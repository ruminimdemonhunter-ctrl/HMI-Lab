#define _POSIX_C_SOURCE 200809L
#include "../l1.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>

enum {
    FAMILY_COUNT = 12, CONTROLLER_COUNT = 4, MAX_SAMPLES = 2400,
    MAX_EVENTS = 256, MAX_SCENARIOS = 60
};
static const uint64_t development_seeds[] = {73, 79};
static const uint64_t training_seeds[] = {1201, 1301, 1409, 1511};
static const uint64_t heldout_seeds[] = {11003, 12007, 13001, 14009, 15013};
static const size_t family_lengths[] =
    {360,600,1000,900,1200,1800,1100,1500,2100,1300,1300,2400};
static const char *family_names[] = {
    "idle","short-cpu-spikes","sustained-cpu","severe-short-memory",
    "memory-near-threshold","memory-during-cpu-oscillation","io-congestion",
    "mixed-priorities","regime-changes","invalid-telemetry",
    "irregular-regressing-time","long-noisy-adaptation"
};
static const char *controller_names[] = {
    "frozen-default","tuned-fixed","v04-legacy-global","v05-risk-aware"
};

/* Filled only after frozen training. Zero values deliberately lock held-out. */
static const uint64_t frozen_training_checksum = UINT64_C(0x981d7ab766c8556c);
static const uint64_t frozen_policy_fingerprint = UINT64_C(0x5147f61840da7089);
static const uint64_t frozen_training_loss_q = UINT64_C(29160);

typedef struct { uint64_t state, increment; } Pcg32;
typedef struct {
    L1Observation observation;
    L1Action truth;
    uint64_t timestamp_ms;
    bool regressing_time;
} Sample;
typedef struct {
    uint64_t seed;
    unsigned family;
    size_t count;
    Sample samples[MAX_SAMPLES];
} Trace;
typedef struct {
    uint64_t samples, fp, missed[4], events[4], undetected[4], switches;
    uint64_t adapt_global, adapt_cpu, adapt_io, invalid, regressions, violations;
    uint8_t global_min, global_max, cpu_min, cpu_max, io_min, io_max;
    uint32_t delays[4][MAX_EVENTS];
    size_t delay_count[4];
    uint64_t scenario_loss_q[MAX_SCENARIOS];
    size_t scenario_count;
    uint64_t loss_q;
} Metrics;
typedef struct {
    Metrics metric;
    uint32_t memory_delay[MAX_EVENTS];
    size_t memory_count;
} ScenarioResult;
typedef struct { Metrics family[FAMILY_COUNT], aggregate; } ControllerMetrics;
typedef struct {
    uint64_t training_checksum, candidate_count, loss_q, fingerprint;
    L1Policy policy;
} Manifest;

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
static uint32_t pcg32_next(Pcg32 *rng) {
    uint64_t old = rng->state;
    uint32_t x = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t rotation = (uint32_t)(old >> 59);
    rng->state = old * UINT64_C(6364136223846793005) + rng->increment;
    return (x >> rotation) | (x << ((-rotation) & 31));
}
static void pcg32_seed(Pcg32 *rng, uint64_t state, uint64_t stream) {
    rng->state = 0; rng->increment = (stream << 1) | 1;
    (void)pcg32_next(rng); rng->state += state; (void)pcg32_next(rng);
}
static int uniform_int(Pcg32 *rng, int low, int high) {
    uint32_t bound = (uint32_t)(high - low + 1);
    uint32_t threshold = (uint32_t)(-bound) % bound, value;
    do value = pcg32_next(rng); while (value < threshold);
    return low + (int)(value % bound);
}
static uint16_t clamp_bp(int value) {
    if (value < 0) return 0;
    if (value > 10000) return 10000;
    return (uint16_t)value;
}
static bool interval(size_t i, int start, int duration) {
    return (int)i >= start && (int)i < start + duration;
}
static L1Action oracle(bool cpu, bool memory, bool io) {
    if (memory) return L1_PROTECT_MEMORY;
    if (io) return L1_DEFER_IO;
    if (cpu) return L1_PREFER_INTERACTIVE;
    return L1_HOLD;
}
static uint64_t fnv_add(uint64_t hash, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) {
        hash ^= (uint8_t)(value >> (8U * i));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
static uint64_t policy_fingerprint(const L1Policy *p) {
    uint64_t h = UINT64_C(1469598103934665603);
    h = fnv_add(h,p->schema_version,4); h = fnv_add(h,p->generation,4);
    h = fnv_add(h,p->cpu_trigger_bp,2); h = fnv_add(h,p->memory_trigger_bp,2);
    h = fnv_add(h,p->io_trigger_bp,2);
    return fnv_add(h,p->confirmation_samples,1);
}
static uint64_t trace_checksum(uint64_t hash, const Trace *trace) {
    hash=fnv_add(hash,trace->seed,8); hash=fnv_add(hash,trace->family,4);
    hash=fnv_add(hash,trace->count,8);
    for (size_t i=0;i<trace->count;i++) {
        const Sample *s=&trace->samples[i];
        hash=fnv_add(hash,s->observation.cpu_some_bp,2);
        hash=fnv_add(hash,s->observation.memory_full_bp,2);
        hash=fnv_add(hash,s->observation.io_full_bp,2);
        hash=fnv_add(hash,s->observation.valid?1:0,1);
        hash=fnv_add(hash,s->truth,1); hash=fnv_add(hash,s->timestamp_ms,8);
        hash=fnv_add(hash,s->regressing_time?1:0,1);
    }
    return hash;
}

static void generate_trace(Trace *trace, uint64_t seed, unsigned family) {
    uint64_t mix=seed^(UINT64_C(0xa0761d6478bd642f)*(family+1));
    Pcg32 truth_rng, noise_rng;
    uint64_t ts=0, accepted=0;
    int starts[4]={0}, durations[4]={0}, spikes[8]={0}, spike_len[8]={0};
    uint64_t a=splitmix64(&mix),b=splitmix64(&mix);
    uint64_t c=splitmix64(&mix),d=splitmix64(&mix);
    pcg32_seed(&truth_rng,a,b); pcg32_seed(&noise_rng,c,d);
    trace->seed=seed; trace->family=family; trace->count=family_lengths[family];
    for (unsigned e=0;e<4;e++) {
        spikes[e*2]=30+(int)e*130+uniform_int(&noise_rng,-8,8);
        spikes[e*2+1]=95+(int)e*130+uniform_int(&noise_rng,-8,8);
        spike_len[e*2]=uniform_int(&noise_rng,1,4);
        spike_len[e*2+1]=uniform_int(&noise_rng,1,4);
    }
    if (family==2) for (unsigned e=0;e<3;e++) {
        starts[e]=120+(int)e*300+uniform_int(&truth_rng,-20,20);
        durations[e]=120+uniform_int(&truth_rng,-20,20);
    }
    if (family==3) for (unsigned e=0;e<4;e++) {
        static const int lens[]={8,12,20,40};
        starts[e]=100+(int)e*200+uniform_int(&truth_rng,-10,10);
        durations[e]=lens[e];
    }
    if (family==4) for (unsigned e=0;e<4;e++) {
        starts[e]=150+(int)e*300+uniform_int(&truth_rng,-15,15);
        durations[e]=80;
    }
    if (family==6) for (unsigned e=0;e<3;e++) {
        static const int bases[]={130,440,760};
        starts[e]=bases[e]+uniform_int(&truth_rng,-20,20);
        durations[e]=150+uniform_int(&truth_rng,-20,20);
    }
    for (size_t i=0;i<trace->count;i++) {
        bool cpu=false,memory=false,io=false,valid=true;
        int cpu_bp=600,mem_bp=10,io_bp=50;
        uint64_t increment=1000;
        if (family==2) for(unsigned e=0;e<3;e++) cpu|=interval(i,starts[e],durations[e]);
        if (family==3) for(unsigned e=0;e<4;e++) memory|=interval(i,starts[e],durations[e]);
        if (family==4) for(unsigned e=0;e<4;e++) memory|=interval(i,starts[e],durations[e]);
        if (family==5) {
            memory=interval(i,700,100)||interval(i,1450,120);
            cpu=interval(i,1100,150);
        }
        if (family==6) for(unsigned e=0;e<3;e++) io|=interval(i,starts[e],durations[e]);
        if (family==7) {
            cpu=interval(i,150,470)||interval(i,1000,320);
            io=interval(i,350,500)||interval(i,1120,280);
            memory=interval(i,540,180);
        }
        if (family==8) {
            io=interval(i,760,140); cpu=interval(i,1320,160);
            memory=interval(i,1750,170);
        }
        if (family==9) {
            cpu=interval(i,250,200); memory=interval(i,720,220);
            valid=!(interval(i,80,18)||interval(i,280,25)||
                    interval(i,760,30)||interval(i,1120,20));
        }
        if (family==10) { io=interval(i,260,220); cpu=interval(i,760,180); }
        if (family==11) {
            cpu=interval(i,1900,120); io=interval(i,2100,130);
            memory=interval(i,2280,60);
        }
        if(cpu) cpu_bp=3600;
        if(memory) mem_bp=family==3?400:(family==4?115:180);
        if(io) io_bp=750;
        if(family==1) for(unsigned e=0;e<8;e++)
            if(interval(i,spikes[e],spike_len[e])) cpu_bp=5200;
        if(family==5 && i<1650 && !cpu) {
            unsigned phase=(unsigned)(i%80);
            cpu_bp=phase<12?3200:(phase<24?2700:600);
        }
        if(family==8) {
            if(i<700) cpu_bp+=700;
            else if(i<1400) {
                io_bp+=180; if(!io && i%95<8) io_bp=650;
            } else {
                cpu_bp+=1100; mem_bp+=18;
                if(!memory && i%120<9) mem_bp=125;
            }
        }
        if(family==11 && i<1850) {
            unsigned phase=(unsigned)(i%100);
            if(phase<14) cpu_bp=3250; else if(phase<28) cpu_bp=2650;
            if(phase>=45&&phase<59) io_bp=620;
            else if(phase>=59&&phase<73) io_bp=340;
        }
        cpu_bp+=uniform_int(&noise_rng,-400,400);
        mem_bp+=uniform_int(&noise_rng,-35,35);
        io_bp+=uniform_int(&noise_rng,-150,150);
        if(family==10) {
            static const uint64_t inc[]={250,500,1000,1750,2500};
            increment=inc[pcg32_next(&noise_rng)%5];
        }
        ts+=increment;
        if(family==10&&(i==370||i==880)) ts-=5000;
        trace->samples[i]=(Sample){
            {clamp_bp(cpu_bp),clamp_bp(mem_bp),clamp_bp(io_bp),valid},
            oracle(cpu,memory,io),ts,i>0&&ts<accepted
        };
        if(ts>=accepted) accepted=ts;
    }
}
static Trace *generate_split(const uint64_t *seeds,size_t seed_count,
                             uint64_t *checksum) {
    size_t count=seed_count*FAMILY_COUNT;
    Trace *traces=calloc(count,sizeof(*traces));
    uint64_t hash=UINT64_C(1469598103934665603);
    if(!traces) return NULL;
    for(size_t s=0;s<seed_count;s++) for(unsigned f=0;f<FAMILY_COUNT;f++) {
        Trace *t=&traces[s*FAMILY_COUNT+f];
        generate_trace(t,seeds[s],f); hash=trace_checksum(hash,t);
    }
    *checksum=hash; return traces;
}

static void metrics_init(Metrics *m) {
    memset(m,0,sizeof(*m)); m->global_min=m->cpu_min=m->io_min=UINT8_MAX;
}
static void controller_init(ControllerMetrics *m) {
    metrics_init(&m->aggregate);
    for(unsigned f=0;f<FAMILY_COUNT;f++) metrics_init(&m->family[f]);
}
static uint64_t loss_q(const Metrics *m) {
    return m->fp*4+m->missed[L1_PREFER_INTERACTIVE]*16+
        m->missed[L1_DEFER_IO]*24+m->missed[L1_PROTECT_MEMORY]*40+m->switches;
}
static void add_metrics(Metrics *d,const Metrics *s) {
    d->samples+=s->samples; d->fp+=s->fp; d->switches+=s->switches;
    d->adapt_global+=s->adapt_global; d->adapt_cpu+=s->adapt_cpu;
    d->adapt_io+=s->adapt_io; d->invalid+=s->invalid;
    d->regressions+=s->regressions; d->violations+=s->violations;
    if(s->global_min<d->global_min)d->global_min=s->global_min;
    if(s->global_max>d->global_max)d->global_max=s->global_max;
    if(s->cpu_min<d->cpu_min)d->cpu_min=s->cpu_min;
    if(s->cpu_max>d->cpu_max)d->cpu_max=s->cpu_max;
    if(s->io_min<d->io_min)d->io_min=s->io_min;
    if(s->io_max>d->io_max)d->io_max=s->io_max;
    for(unsigned a=1;a<=3;a++) {
        d->missed[a]+=s->missed[a]; d->events[a]+=s->events[a];
        d->undetected[a]+=s->undetected[a];
        for(size_t i=0;i<s->delay_count[a];i++) {
            if(d->delay_count[a]>=MAX_EVENTS) exit(2);
            d->delays[a][d->delay_count[a]++]=s->delays[a][i];
        }
    }
    d->loss_q+=s->loss_q;
    if(d->scenario_count>=MAX_SCENARIOS) exit(2);
    d->scenario_loss_q[d->scenario_count++]=s->loss_q;
}
static L1AdaptationMode controller_mode(unsigned c) {
    if(c<2) return L1_ADAPT_DISABLED;
    return c==2?L1_ADAPT_LEGACY_GLOBAL:L1_ADAPT_RISK_AWARE;
}
static ScenarioResult run_trace(const Trace *trace,const L1Policy *policy,
                                unsigned controller) {
    ScenarioResult out; L1Core core; L1Action previous=L1_HOLD,event=L1_HOLD;
    size_t event_start=0; bool detected=false;
    memset(&out,0,sizeof(out)); metrics_init(&out.metric); l1_init(&core);
    if(policy && !l1_apply_policy(&core,policy)) exit(2);
    if(!l1_set_adaptation_mode(&core,controller_mode(controller))) exit(2);
    out.metric.global_min=core.debounce_extra;
    out.metric.cpu_min=core.cpu_debounce_extra; out.metric.io_min=core.io_debounce_extra;
    for(size_t i=0;i<trace->count;i++) {
        const Sample *s=&trace->samples[i];
        uint8_t old_g=core.debounce_extra,old_c=core.cpu_debounce_extra;
        uint8_t old_io=core.io_debounce_extra;
        if(s->truth!=event) {
            if(event!=L1_HOLD&&!detected) {
                out.metric.undetected[event]++;
                if(event==L1_PROTECT_MEMORY)
                    out.memory_delay[out.memory_count++]=UINT32_MAX;
            }
            event=s->truth; event_start=i; detected=event==L1_HOLD;
            if(event!=L1_HOLD) out.metric.events[event]++;
        }
        L1Action actual=l1_step_at(&core,s->observation,s->timestamp_ms);
        out.metric.samples++;
        if(!s->observation.valid)out.metric.invalid++;
        if(s->regressing_time)out.metric.regressions++;
        if((!s->observation.valid||s->regressing_time)&&actual!=L1_HOLD)
            out.metric.violations++;
        if(s->truth==L1_HOLD) { if(actual!=L1_HOLD)out.metric.fp++; }
        else {
            if(actual!=s->truth)out.metric.missed[s->truth]++;
            if(!detected&&actual==s->truth) {
                uint32_t delay=(uint32_t)(i-event_start);
                out.metric.delays[s->truth][out.metric.delay_count[s->truth]++]=delay;
                if(s->truth==L1_PROTECT_MEMORY)
                    out.memory_delay[out.memory_count++]=delay;
                detected=true;
            }
        }
        if(i&&actual!=previous)out.metric.switches++;
        previous=actual;
        if(core.debounce_extra!=old_g)out.metric.adapt_global++;
        if(core.cpu_debounce_extra!=old_c)out.metric.adapt_cpu++;
        if(core.io_debounce_extra!=old_io)out.metric.adapt_io++;
        if(core.debounce_extra<out.metric.global_min)out.metric.global_min=core.debounce_extra;
        if(core.debounce_extra>out.metric.global_max)out.metric.global_max=core.debounce_extra;
        if(core.cpu_debounce_extra<out.metric.cpu_min)out.metric.cpu_min=core.cpu_debounce_extra;
        if(core.cpu_debounce_extra>out.metric.cpu_max)out.metric.cpu_max=core.cpu_debounce_extra;
        if(core.io_debounce_extra<out.metric.io_min)out.metric.io_min=core.io_debounce_extra;
        if(core.io_debounce_extra>out.metric.io_max)out.metric.io_max=core.io_debounce_extra;
        if(core.debounce_extra>L1_MAX_EXTRA_CONFIRMATIONS||
           core.cpu_debounce_extra>L1_MAX_EXTRA_CONFIRMATIONS||
           core.io_debounce_extra>L1_MAX_EXTRA_CONFIRMATIONS) exit(2);
    }
    if(event!=L1_HOLD&&!detected) {
        out.metric.undetected[event]++;
        if(event==L1_PROTECT_MEMORY)out.memory_delay[out.memory_count++]=UINT32_MAX;
    }
    out.metric.loss_q=loss_q(&out.metric); return out;
}

static int cmp_u64(const void *a,const void *b) {
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b; return (x>y)-(x<y);
}
static int cmp_u32(const void *a,const void *b) {
    uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b; return (x>y)-(x<y);
}
static void distribution(const uint64_t *v,size_t n,uint64_t *min,
                         uint64_t *median,uint64_t *max) {
    uint64_t c[MAX_SCENARIOS]; memcpy(c,v,n*sizeof(*c));
    qsort(c,n,sizeof(*c),cmp_u64); *min=c[0];*median=c[(n-1)/2];*max=c[n-1];
}
static void delays(const Metrics *m,L1Action a,double *mean,
                   uint32_t *p95,uint32_t *max) {
    size_t n=m->delay_count[a]; uint32_t c[MAX_EVENTS]; uint64_t sum=0;
    if(!n){*mean=0;*p95=*max=0;return;}
    memcpy(c,m->delays[a],n*sizeof(*c));qsort(c,n,sizeof(*c),cmp_u32);
    for(size_t i=0;i<n;i++)sum+=c[i];
    *mean=(double)sum/n;*p95=c[((95*n+99)/100)-1];*max=c[n-1];
}
static void print_header(void) {
    puts("HEADER,split,controller,family,scenarios,samples,loss,scenario_min,"
         "scenario_median,scenario_max,fp,miss_cpu,miss_io,miss_memory,"
         "events_cpu,events_io,events_memory,undetected_cpu,undetected_io,"
         "undetected_memory,cpu_delay_mean,cpu_p95,cpu_max,io_delay_mean,"
         "io_p95,io_max,memory_delay_mean,memory_p95,memory_max,switches,"
         "adapt_global,adapt_cpu,adapt_io,global_range,cpu_range,io_range,"
         "invalid,regressions,fallback_violations,cpu_ns_per_decision,rss_kib");
}
static void print_row(const char *split,const char *controller,const char *family,
                      const Metrics *m,double ns,long rss) {
    uint64_t mn,med,mx; double cm,im,mm; uint32_t cp,cx,ip,ix,mp,mxdelay;
    distribution(m->scenario_loss_q,m->scenario_count,&mn,&med,&mx);
    delays(m,L1_PREFER_INTERACTIVE,&cm,&cp,&cx);
    delays(m,L1_DEFER_IO,&im,&ip,&ix);delays(m,L1_PROTECT_MEMORY,&mm,&mp,&mxdelay);
    printf("RESULT,%s,%s,%s,%zu,%"PRIu64",%.2f,%.2f,%.2f,%.2f,"
      "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64","
      "%"PRIu64",%"PRIu64",%"PRIu64",%.3f,%u,%u,%.3f,%u,%u,%.3f,%u,%u,"
      "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%u..%u,%u..%u,%u..%u,"
      "%"PRIu64",%"PRIu64",%"PRIu64",%.3f,%ld\n",
      split,controller,family,m->scenario_count,m->samples,m->loss_q/4.0,
      mn/4.0,med/4.0,mx/4.0,m->fp,m->missed[1],m->missed[3],m->missed[2],
      m->events[1],m->events[3],m->events[2],m->undetected[1],m->undetected[3],
      m->undetected[2],cm,cp,cx,im,ip,ix,mm,mp,mxdelay,m->switches,
      m->adapt_global,m->adapt_cpu,m->adapt_io,
      m->global_min==UINT8_MAX?0:m->global_min,m->global_max,
      m->cpu_min==UINT8_MAX?0:m->cpu_min,m->cpu_max,
      m->io_min==UINT8_MAX?0:m->io_min,m->io_max,
      m->invalid,m->regressions,m->violations,ns,rss);
}

static bool better(const Metrics *a,const L1Policy *ap,
                   const Metrics *b,const L1Policy *bp) {
    if(a->loss_q!=b->loss_q)return a->loss_q<b->loss_q;
#define LOWER(field) if(a->field!=b->field)return a->field<b->field
    LOWER(missed[2]);LOWER(undetected[2]);LOWER(missed[3]);LOWER(undetected[3]);
    LOWER(missed[1]);LOWER(undetected[1]);LOWER(fp);LOWER(switches);
#undef LOWER
    if(ap->confirmation_samples!=bp->confirmation_samples)
        return ap->confirmation_samples<bp->confirmation_samples;
    if(ap->cpu_trigger_bp!=bp->cpu_trigger_bp)return ap->cpu_trigger_bp<bp->cpu_trigger_bp;
    if(ap->memory_trigger_bp!=bp->memory_trigger_bp)return ap->memory_trigger_bp<bp->memory_trigger_bp;
    return ap->io_trigger_bp<bp->io_trigger_bp;
}
static bool write_manifest(const char *path,const Manifest *m) {
    FILE *f=fopen(path,"w"); if(!f)return false;
    int ok=fprintf(f,"protocol=l1-v0.5\ntraining_checksum=%016"PRIx64"\n"
      "candidate_count=%"PRIu64"\nloss_quarters=%"PRIu64"\n"
      "schema_version=%u\ngeneration=%u\ncpu_trigger_bp=%u\n"
      "memory_trigger_bp=%u\nio_trigger_bp=%u\nconfirmation_samples=%u\n"
      "policy_fingerprint=%016"PRIx64"\n",m->training_checksum,
      m->candidate_count,m->loss_q,m->policy.schema_version,m->policy.generation,
      m->policy.cpu_trigger_bp,m->policy.memory_trigger_bp,m->policy.io_trigger_bp,
      m->policy.confirmation_samples,m->fingerprint)>0;
    if(fclose(f)!=0) ok=0;
    return ok!=0;
}
static bool parse_hex(const char *s,uint64_t *v) {
    char *end; errno=0; unsigned long long x=strtoull(s,&end,16);
    if(*s<'0'||(*s>'9'&&*s<'A')||(*s>'F'&&*s<'a')||*s>'f'||
       errno||end==s||*end) return false;
    *v=(uint64_t)x;
    return true;
}
static bool parse_dec(const char *s,uint64_t *v) {
    char *end; errno=0; unsigned long long x=strtoull(s,&end,10);
    if(*s<'0'||*s>'9'||errno||end==s||*end) return false;
    *v=(uint64_t)x;
    return true;
}
static bool read_value(FILE *f,const char *key,char *value,size_t size) {
    char line[160];if(!fgets(line,sizeof(line),f))return false;
    char *nl=strchr(line,'\n');if(nl)*nl='\0';
    size_t n=strlen(key);if(strncmp(line,key,n)||line[n]!='=')return false;
    if(strlen(line+n+1)>=size) return false;
    strcpy(value,line+n+1);
    return true;
}
static bool read_manifest(const char *path,Manifest *m) {
    FILE *f=fopen(path,"r");char v[80];uint64_t x=0;bool ok=f!=NULL;
#define READ(key) (ok=ok&&read_value(f,key,v,sizeof(v)))
    if(READ("protocol"))ok=!strcmp(v,"l1-v0.5");
    if(READ("training_checksum"))ok=parse_hex(v,&m->training_checksum);
    if(READ("candidate_count"))ok=parse_dec(v,&m->candidate_count);
    if(READ("loss_quarters"))ok=parse_dec(v,&m->loss_q);
    if(READ("schema_version")){ok=parse_dec(v,&x)&&x<=UINT32_MAX;m->policy.schema_version=(uint32_t)x;}
    if(READ("generation")){ok=parse_dec(v,&x)&&x<=UINT32_MAX;m->policy.generation=(uint32_t)x;}
    if(READ("cpu_trigger_bp")){ok=parse_dec(v,&x)&&x<=UINT16_MAX;m->policy.cpu_trigger_bp=(uint16_t)x;}
    if(READ("memory_trigger_bp")){ok=parse_dec(v,&x)&&x<=UINT16_MAX;m->policy.memory_trigger_bp=(uint16_t)x;}
    if(READ("io_trigger_bp")){ok=parse_dec(v,&x)&&x<=UINT16_MAX;m->policy.io_trigger_bp=(uint16_t)x;}
    if(READ("confirmation_samples")){ok=parse_dec(v,&x)&&x<=UINT8_MAX;m->policy.confirmation_samples=(uint8_t)x;}
    if(READ("policy_fingerprint"))ok=parse_hex(v,&m->fingerprint);
#undef READ
    if(ok){int ch=fgetc(f);ok=ch==EOF&&!ferror(f);}
    if(f&&fclose(f)!=0)ok=false;
    return ok&&l1_policy_valid(&m->policy)&&m->fingerprint==policy_fingerprint(&m->policy);
}
static int train(const char *path) {
    static const uint16_t cv[]={2000,3000,4000},mv[]={50,100,200},iv[]={300,500,800};
    static const uint8_t kv[]={2,3,4,5};
    uint64_t checksum;size_t count=sizeof(training_seeds)/8*FAMILY_COUNT;
    Trace *traces=generate_split(training_seeds,sizeof(training_seeds)/8,&checksum);
    Metrics best;L1Policy bestp={0};bool have=false;uint64_t candidates=0;
    if(!traces) return 2;
    metrics_init(&best);
    for(size_t c=0;c<3;c++)for(size_t m=0;m<3;m++)for(size_t io=0;io<3;io++)
    for(size_t k=0;k<4;k++) {
        L1Policy p={1,1,cv[c],mv[m],iv[io],kv[k]};Metrics total;metrics_init(&total);
        candidates++;
        for(size_t i=0;i<count;i++){ScenarioResult r=run_trace(&traces[i],&p,1);add_metrics(&total,&r.metric);}
        if(!have||better(&total,&p,&best,&bestp)){best=total;bestp=p;have=true;}
    }
    Manifest manifest={checksum,candidates,best.loss_q,policy_fingerprint(&bestp),bestp};
    if(!write_manifest(path,&manifest)){free(traces);return 2;}
    printf("TRAIN_V05,candidates=%"PRIu64",checksum=%016"PRIx64",loss=%.2f,"
      "cpu=%u,memory=%u,io=%u,confirmation=%u,fingerprint=%016"PRIx64",manifest=%s\n",
      candidates,checksum,best.loss_q/4.0,bestp.cpu_trigger_bp,
      bestp.memory_trigger_bp,bestp.io_trigger_bp,bestp.confirmation_samples,
      manifest.fingerprint,path);
    free(traces);return 0;
}
static int development(void) {
    uint64_t checksum,invalid=0,regress=0;size_t seeds=sizeof(development_seeds)/8;
    Trace *t=generate_split(development_seeds,seeds,&checksum);if(!t)return 2;
    for(size_t i=0;i<seeds*FAMILY_COUNT;i++)for(size_t j=0;j<t[i].count;j++){
        if(!t[i].samples[j].observation.valid)invalid++;
        if(t[i].samples[j].regressing_time)regress++;
    }
    printf("DEVELOPMENT_V05,scenarios=%zu,checksum=%016"PRIx64",invalid=%"PRIu64",regressions=%"PRIu64"\n",
      seeds*FAMILY_COUNT,checksum,invalid,regress);free(t);
    return invalid&&regress?0:2;
}
static uint64_t ns_between(struct timespec a,struct timespec b) {
    int64_t seconds=(int64_t)b.tv_sec-(int64_t)a.tv_sec;
    int64_t nanoseconds=(int64_t)b.tv_nsec-(int64_t)a.tv_nsec;
    return (uint64_t)(seconds*INT64_C(1000000000)+nanoseconds);
}
static uint32_t max_delay(const Metrics *m,L1Action a) {
    uint32_t x=0;for(size_t i=0;i<m->delay_count[a];i++)if(m->delays[a][i]>x)x=m->delays[a][i];return x;
}
static int heldout(const char *manifest_path,unsigned repetitions) {
    Manifest manifest={0};uint64_t checksum,elapsed[4]={0};
    size_t seeds=sizeof(heldout_seeds)/8,count=seeds*FAMILY_COUNT;
    if(!read_manifest(manifest_path,&manifest)){fputs("invalid V0.5 manifest\n",stderr);return 2;}
    if(!frozen_training_checksum||manifest.training_checksum!=frozen_training_checksum||
       manifest.candidate_count!=108||manifest.loss_q!=frozen_training_loss_q||
       manifest.fingerprint!=frozen_policy_fingerprint) {
        fputs("V0.5 manifest does not match frozen training identity\n",stderr);return 2;
    }
    Trace *traces=generate_split(heldout_seeds,seeds,&checksum);if(!traces)return 2;
    ControllerMetrics first[4];for(unsigned c=0;c<4;c++)controller_init(&first[c]);
    ScenarioResult *stored=calloc(CONTROLLER_COUNT*count,sizeof(*stored));if(!stored){free(traces);return 2;}
    for(unsigned rep=0;rep<repetitions;rep++)for(unsigned order=0;order<4;order++){
        unsigned c=(order+rep)%4;struct timespec a,b;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&a);
        for(size_t i=0;i<count;i++){
            const L1Policy *p=c?&manifest.policy:NULL;
            ScenarioResult r=run_trace(&traces[i],p,c);
            if(rep==0){stored[c*count+i]=r;add_metrics(&first[c].family[traces[i].family],&r.metric);add_metrics(&first[c].aggregate,&r.metric);}
        }
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&b);elapsed[c]+=ns_between(a,b);
    }
    uint64_t paired=0,equal=0,faster=0,slower=0;uint32_t worst_added=0;
    bool family_memory_ok[FAMILY_COUNT];for(unsigned f=0;f<FAMILY_COUNT;f++)family_memory_ok[f]=true;
    for(size_t i=0;i<count;i++){
        ScenarioResult *fixed=&stored[1*count+i],*risk=&stored[3*count+i];
        if(fixed->memory_count!=risk->memory_count){free(stored);free(traces);return 2;}
        for(size_t e=0;e<fixed->memory_count;e++){
            uint32_t fd=fixed->memory_delay[e],rd=risk->memory_delay[e];paired++;
            if(rd==fd)equal++;else if(rd<fd)faster++;else{
                slower++;uint32_t added=fd==UINT32_MAX?0:(rd==UINT32_MAX?UINT32_MAX:rd-fd);
                if(added>worst_added)worst_added=added;
                printf("MEMORY_REGRESSION,seed=%"PRIu64",family=%s,event=%zu,fixed=%u,risk=%u\n",
                  traces[i].seed,family_names[traces[i].family],e,fd,rd);
            }
        }
    }
    for(unsigned f=0;f<FAMILY_COUNT;f++)
        family_memory_ok[f]=first[3].family[f].missed[2]<=first[1].family[f].missed[2];
    struct rusage usage;struct utsname system;getrusage(RUSAGE_SELF,&usage);uname(&system);
    printf("RUN_V05,checksum=%016"PRIx64",repetitions=%u,training_checksum=%016"PRIx64","
      "policy=%u|%u|%u|%u,fingerprint=%016"PRIx64"\n",checksum,repetitions,
      manifest.training_checksum,manifest.policy.cpu_trigger_bp,manifest.policy.memory_trigger_bp,
      manifest.policy.io_trigger_bp,manifest.policy.confirmation_samples,manifest.fingerprint);
    printf("ENV,compiler=%s,system=%s,machine=%s,sizeof_l1core=%zu,state_bytes=72,rss_kib=%ld\n",
      __VERSION__,system.sysname,system.machine,sizeof(L1Core),usage.ru_maxrss);
    print_header();
    for(unsigned c=0;c<4;c++){
        double ns=(double)elapsed[c]/(first[c].aggregate.samples*repetitions);
        for(unsigned f=0;f<FAMILY_COUNT;f++)print_row("heldout",controller_names[c],family_names[f],&first[c].family[f],ns,usage.ru_maxrss);
        print_row("heldout",controller_names[c],"ALL",&first[c].aggregate,ns,usage.ru_maxrss);
    }
    bool family_ok=true;for(unsigned f=0;f<FAMILY_COUNT;f++)family_ok&=family_memory_ok[f];
    bool memory_ok=family_ok&&slower==0&&first[3].aggregate.missed[2]<=first[1].aggregate.missed[2]&&
      first[3].aggregate.undetected[2]<=first[1].aggregate.undetected[2]&&
      max_delay(&first[3].aggregate,L1_PROTECT_MEMORY)<=max_delay(&first[1].aggregate,L1_PROTECT_MEMORY);
    bool io_ok=first[3].aggregate.missed[3]<=first[1].aggregate.missed[3]&&
      first[3].aggregate.undetected[3]<=first[1].aggregate.undetected[3]&&
      max_delay(&first[3].aggregate,L1_DEFER_IO)<=max_delay(&first[1].aggregate,L1_DEFER_IO);
    uint64_t violations=0;for(unsigned c=0;c<4;c++)violations+=first[c].aggregate.violations;
    bool go=memory_ok&&!violations&&first[3].aggregate.loss_q<=first[1].aggregate.loss_q&&
      first[3].aggregate.switches<first[1].aggregate.switches&&
      first[3].aggregate.loss_q<first[2].aggregate.loss_q;
    printf("MEMORY_PAIRED,count=%"PRIu64",equal=%"PRIu64",faster=%"PRIu64",slower=%"PRIu64",worst_added=%u\n",
      paired,equal,faster,slower,worst_added);
    printf("GUARDRAIL,memory=%s,io=%s,fallback_violations=%"PRIu64"\n",
      memory_ok?"PASS":"REGRESSION",io_ok?"PASS":"UNFAVORABLE",violations);
    printf("GO_NO_GO,%s\n",go?"RESEARCH_GO":"NO_GO");
    free(stored);free(traces);return violations?2:0;
}
static bool repetitions(const char *s,unsigned *out) {
    char *end;if(!s||!*s||*s=='-')return false;errno=0;unsigned long x=strtoul(s,&end,10);
    if(errno||*end||x<1||x>10000) return false;
    *out=(unsigned)x;
    return true;
}
int main(int argc,char **argv) {
    unsigned reps;
    if(argc==2&&!strcmp(argv[1],"development"))return development();
    if(argc==3&&!strcmp(argv[1],"train"))return train(argv[2]);
    if(argc==4&&!strcmp(argv[1],"heldout")&&repetitions(argv[3],&reps))
        return heldout(argv[2],reps);
    fprintf(stderr,"usage: %s development | train MANIFEST | heldout MANIFEST REPETITIONS\n",argv[0]);
    return 2;
}
