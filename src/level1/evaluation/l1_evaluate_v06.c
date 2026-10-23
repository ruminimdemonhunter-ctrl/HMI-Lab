#define _POSIX_C_SOURCE 200809L
#include "../l1.h"
#include "../l1_shadow.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>

enum {
    FAMILY_COUNT=12, FORECAST_COUNT=3, ACTION_COUNT=4,
    MAX_SAMPLES=4800, MAX_DELAYS=768, MAX_LEADS=6000,
    MAX_SCENARIOS=60
};
static const uint64_t development_seeds[]={83,89};
static const uint64_t training_seeds[]={1601,1709,1801,1907};
static const uint64_t heldout_seeds[]={21001,22003,23003,24007,25013};
static const size_t family_lengths[]={1800,1800,2400,2400,2400,2600,
                                      3600,3000,2400,2400,2400,4800};
static const char *family_names[]={
    "idle","short-observation-noise","imminent-cpu","memory-urgency",
    "io-urgency","simultaneous-priorities","regime-shifts",
    "delayed-inconsistent-outcomes","invalid-and-gaps",
    "counter-reset-corruption","irregular-regressing-time",
    "long-drift-calibration"
};
static const char *forecast_names[]={"always-negative","tuned-static",
                                     "outcome-aware-shadow"};
static const char *action_names[]={"frozen-default","tuned-fixed",
                                   "v05-risk-aware","v06-shadow"};

/* Filled only after frozen development/training output is committed. */
static const uint64_t frozen_development_checksum=
    UINT64_C(0xadafe2486842d831);
static const uint64_t frozen_training_checksum=
    UINT64_C(0x69e07b6e49c323f6);
static const uint64_t frozen_manifest_fingerprint=
    UINT64_C(0x12bbce7fc6b50db7);
static const uint64_t frozen_action_loss_q=UINT64_C(56568);
static const uint64_t frozen_forecast_errors=UINT64_C(333);
static const uint64_t frozen_forecast_windows=UINT64_C(2084);

typedef struct {uint64_t state,increment;} Pcg32;
typedef struct {
    L1Observation observation;
    L1PressureCounters counters;
    L1Action truth;
    uint8_t latent_mask;
    uint64_t timestamp_ms;
    bool regressing;
    bool injected_reset;
    bool injected_corruption;
} Sample;
typedef struct {uint64_t seed;unsigned family;size_t count;Sample *samples;} Trace;

typedef struct {
    uint64_t issued,evaluated,tp[3],tn[3],fp[3],fn[3];
    uint64_t discard_invalid,discard_gap_late,discard_reset,resets,unfinished;
    uint64_t updates[3];
    int8_t offset_min[3],offset_max[3];
    uint64_t leads[3][MAX_LEADS];size_t lead_count[3];
} ForecastMetrics;
typedef struct {ForecastMetrics predictor[FORECAST_COUNT];double scenario_brier[FORECAST_COUNT];} ForecastResult;

typedef struct {
    uint64_t samples,fp,missed[4],events[4],undetected[4],switches,violations;
    uint32_t delays[4][MAX_DELAYS];size_t delay_count[4];uint64_t loss_q;
    uint32_t event_delays[4][MAX_DELAYS];size_t event_delay_count[4];
} ActionMetrics;
typedef struct {
    uint64_t development_checksum,training_checksum,action_loss_q;
    uint64_t forecast_errors,forecast_windows,fingerprint;
    L1Policy policy;L1ForecastThresholds forecast;
} Manifest;

static uint64_t splitmix64(uint64_t *state){
    uint64_t z=(*state+=UINT64_C(0x9e3779b97f4a7c15));
    z=(z^(z>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    z=(z^(z>>27))*UINT64_C(0x94d049bb133111eb);return z^(z>>31);
}
static uint32_t pcg32_next(Pcg32 *r){uint64_t old=r->state;uint32_t x=(uint32_t)(((old>>18)^old)>>27),rot=(uint32_t)(old>>59);r->state=old*UINT64_C(6364136223846793005)+r->increment;return(x>>rot)|(x<<((-rot)&31));}
static void pcg32_seed(Pcg32 *r,uint64_t state,uint64_t stream){r->state=0;r->increment=(stream<<1)|1;(void)pcg32_next(r);r->state+=state;(void)pcg32_next(r);}
static int uniform_int(Pcg32 *r,int low,int high){uint32_t bound=(uint32_t)(high-low+1),threshold=(uint32_t)(-bound)%bound,v;do v=pcg32_next(r);while(v<threshold);return low+(int)(v%bound);}
static uint16_t clamp_bp(int value){if(value<0)return 0;if(value>10000)return 10000;return(uint16_t)value;}
static bool interval(size_t i,int start,int length){return(int)i>=start&&(int)i<start+length;}
static uint64_t sat_add(uint64_t a,uint64_t b){return UINT64_MAX-a<b?UINT64_MAX:a+b;}
static uint64_t fnv_add(uint64_t h,uint64_t v,unsigned bytes){for(unsigned i=0;i<bytes;i++){h^=(uint8_t)(v>>(8U*i));h*=UINT64_C(1099511628211);}return h;}
static uint64_t policy_fingerprint(const L1Policy *p){uint64_t h=UINT64_C(1469598103934665603);h=fnv_add(h,p->schema_version,4);h=fnv_add(h,p->generation,4);h=fnv_add(h,p->cpu_trigger_bp,2);h=fnv_add(h,p->memory_trigger_bp,2);h=fnv_add(h,p->io_trigger_bp,2);return fnv_add(h,p->confirmation_samples,1);}
static uint64_t manifest_fingerprint(const Manifest *m){uint64_t h=policy_fingerprint(&m->policy);h=fnv_add(h,m->development_checksum,8);h=fnv_add(h,m->training_checksum,8);h=fnv_add(h,m->action_loss_q,8);h=fnv_add(h,m->forecast_errors,8);h=fnv_add(h,m->forecast_windows,8);h=fnv_add(h,m->forecast.cpu_bp,2);h=fnv_add(h,m->forecast.memory_bp,2);return fnv_add(h,m->forecast.io_bp,2);}

static uint8_t latent_for(unsigned family,size_t i){
    bool cpu=false,memory=false,io=false;
    if(family==2){static const int s[]={300,650,1000,1350,1700,2050};for(unsigned e=0;e<6;e++)cpu|=interval(i,s[e],100);}
    if(family==3){static const int s[]={200,500,900,1300,1700,2100};static const int d[]={20,80,150,30,120,60};for(unsigned e=0;e<6;e++)memory|=interval(i,s[e],d[e]);cpu=interval(i,450,180);io=interval(i,1250,170);}
    if(family==4){static const int s[]={180,520,860,1220,1600,2050};static const int d[]={30,100,160,40,130,80};for(unsigned e=0;e<6;e++)io|=interval(i,s[e],d[e]);cpu=interval(i,760,190);}
    if(family==5){cpu=interval(i,250,700)||interval(i,1500,600);io=interval(i,500,700)||interval(i,1750,500);memory=interval(i,750,220)||interval(i,1950,180);}
    if(family==6){cpu=interval(i,1200,1200);io=interval(i,1500,700);memory=interval(i,1900,250);}
    if(family==7){cpu=interval(i,650,180)||interval(i,1850,220);io=interval(i,1050,190);memory=interval(i,2450,140);}
    if(family==8){cpu=interval(i,300,250);memory=interval(i,1100,180);io=interval(i,1750,220);}
    if(family==9){cpu=interval(i,300,220);io=interval(i,900,240);memory=interval(i,1700,180);}
    if(family==10){io=interval(i,350,250);cpu=interval(i,1050,220);memory=interval(i,1850,180);}
    if(family==11){unsigned phase=(unsigned)(i%900);cpu=phase>=180&&phase<420;io=phase>=380&&phase<650;memory=phase>=610&&phase<720;}
    return(uint8_t)((cpu?1U:0U)|(memory?2U:0U)|(io?4U:0U));
}
static L1Action oracle(uint8_t mask){if(mask&2U)return L1_PROTECT_MEMORY;if(mask&4U)return L1_DEFER_IO;if(mask&1U)return L1_PREFER_INTERACTIVE;return L1_HOLD;}

static void generate_trace(Trace *trace,uint64_t seed,unsigned family){
    uint64_t mix=seed^(UINT64_C(0xd6e8feb86659fd93)*(family+1));
    Pcg32 truth_rng,noise_rng,outcome_rng,timing_rng;
    pcg32_seed(&truth_rng,splitmix64(&mix),splitmix64(&mix));
    pcg32_seed(&noise_rng,splitmix64(&mix),splitmix64(&mix));
    pcg32_seed(&outcome_rng,splitmix64(&mix),splitmix64(&mix));
    pcg32_seed(&timing_rng,splitmix64(&mix),splitmix64(&mix));
    (void)truth_rng;
    uint64_t ts=0,accepted_ts=0,totals[3]={0,0,0};int filtered[3]={600,10,50};
    int spike_start[10],spike_len[10];for(unsigned e=0;e<10;e++){spike_start[e]=80+(int)e*160+uniform_int(&noise_rng,-12,12);spike_len[e]=uniform_int(&noise_rng,1,8);}
    trace->seed=seed;trace->family=family;trace->count=family_lengths[family];
    for(size_t i=0;i<trace->count;i++){
        uint64_t increment_ms=1000;
        if(family==10){static const uint64_t inc[]={250,500,1000,1750,2500};increment_ms=inc[pcg32_next(&timing_rng)%5];}
        if(family==8&&(i==600||i==1500))increment_ms=7000;
        if(i){ts+=increment_ms;if(family==10&&(i==800||i==1600))ts-=4000;}
        uint8_t latent=latent_for(family,i);int action_target[3]={600,10,50};int rates[3]={100,2,10};
        if(latent&1U){action_target[0]=3600;rates[0]=1800;}
        if(latent&2U){action_target[1]=180;rates[1]=120;}
        if(latent&4U){action_target[2]=750;rates[2]=450;}
        if(family==6){if(i<1200){rates[0]=200;rates[2]=20;}else if(i>=2400){rates[0]=80;rates[1]=1;rates[2]=8;}}
        if(family==7){
            if(interval(i,500,120)||interval(i,1650,120)){action_target[0]=2600;action_target[2]=500;}
            if(interval(i,620,30)||interval(i,1020,30)||interval(i,1820,30)||interval(i,2420,30)){action_target[0]=300;action_target[1]=2;action_target[2]=20;}
        }
        if(family==2)for(unsigned e=0;e<6;e++){int start=300+(int)e*350;if((int)i>=start-30&&(int)i<start)action_target[0]=600+((int)i-(start-30))*100;}
        for(unsigned c=0;c<3;c++)filtered[c]+=(action_target[c]-filtered[c])/10;
        int observed[3]={filtered[0]+uniform_int(&noise_rng,-300,300),filtered[1]+uniform_int(&noise_rng,-20,20),filtered[2]+uniform_int(&noise_rng,-80,80)};
        if(family==1)for(unsigned e=0;e<10;e++)if(interval(i,spike_start[e],spike_len[e])){observed[e%3]=e%3==0?3500:(e%3==1?200:900);}
        if(i){for(unsigned c=0;c<3;c++){int noisy_rate=rates[c]+uniform_int(&outcome_rng,-(rates[c]/5+1),rates[c]/5+1);if(noisy_rate<0)noisy_rate=0;uint64_t elapsed_us=increment_ms*1000U;uint64_t delta=(uint64_t)noisy_rate*elapsed_us/10000U;totals[c]=sat_add(totals[c],delta);}}
        bool reset=false,corrupt=false;
        if(family==9&&(i==700||i==1450)){totals[0]=10;totals[1]=20;totals[2]=30;reset=true;}
        if(family==9&&i==1300){totals[2]=sat_add(totals[2],increment_ms*2000U);corrupt=true;}
        bool valid=true;if(family==8&&(interval(i,180,25)||interval(i,920,40)||interval(i,1800,35)))valid=false;
        bool regressing=i>0&&ts<accepted_ts;
        trace->samples[i]=(Sample){
            {clamp_bp(observed[0]),clamp_bp(observed[1]),clamp_bp(observed[2]),valid},
            {totals[0],totals[1],totals[2],valid},oracle(latent),latent,ts,
            regressing,reset,corrupt};
        if(ts>=accepted_ts)accepted_ts=ts;
    }
}
static uint64_t trace_checksum(uint64_t h,const Trace *t){h=fnv_add(h,t->seed,8);h=fnv_add(h,t->family,4);h=fnv_add(h,t->count,8);for(size_t i=0;i<t->count;i++){const Sample*s=&t->samples[i];h=fnv_add(h,s->observation.cpu_some_bp,2);h=fnv_add(h,s->observation.memory_full_bp,2);h=fnv_add(h,s->observation.io_full_bp,2);h=fnv_add(h,s->observation.valid,1);h=fnv_add(h,s->counters.cpu_some_total_us,8);h=fnv_add(h,s->counters.memory_full_total_us,8);h=fnv_add(h,s->counters.io_full_total_us,8);h=fnv_add(h,s->truth,1);h=fnv_add(h,s->latent_mask,1);h=fnv_add(h,s->timestamp_ms,8);h=fnv_add(h,s->regressing,1);h=fnv_add(h,s->injected_reset,1);h=fnv_add(h,s->injected_corruption,1);}return h;}
static void free_split(Trace *traces,size_t count){if(!traces)return;for(size_t i=0;i<count;i++)free(traces[i].samples);free(traces);}
static Trace *generate_split(const uint64_t *seeds,size_t seed_count,uint64_t *checksum){size_t n=seed_count*FAMILY_COUNT;Trace*t=calloc(n,sizeof(*t));uint64_t h=UINT64_C(1469598103934665603);if(!t)return NULL;for(size_t s=0;s<seed_count;s++)for(unsigned f=0;f<FAMILY_COUNT;f++){Trace*one=&t[s*FAMILY_COUNT+f];one->samples=calloc(family_lengths[f],sizeof(*one->samples));if(!one->samples){free_split(t,n);return NULL;}generate_trace(one,seeds[s],f);h=trace_checksum(h,one);}*checksum=h;return t;}

static void forecast_init(ForecastMetrics*m){memset(m,0,sizeof(*m));for(unsigned c=0;c<3;c++){m->offset_min[c]=INT8_MAX;m->offset_max[c]=INT8_MIN;}}
static void forecast_add(ForecastMetrics*d,const ForecastMetrics*s){d->issued+=s->issued;d->evaluated+=s->evaluated;d->discard_invalid+=s->discard_invalid;d->discard_gap_late+=s->discard_gap_late;d->discard_reset+=s->discard_reset;d->resets+=s->resets;d->unfinished+=s->unfinished;for(unsigned c=0;c<3;c++){d->tp[c]+=s->tp[c];d->tn[c]+=s->tn[c];d->fp[c]+=s->fp[c];d->fn[c]+=s->fn[c];d->updates[c]+=s->updates[c];if(s->offset_min[c]<d->offset_min[c])d->offset_min[c]=s->offset_min[c];if(s->offset_max[c]>d->offset_max[c])d->offset_max[c]=s->offset_max[c];for(size_t i=0;i<s->lead_count[c];i++){if(d->lead_count[c]>=MAX_LEADS)exit(2);d->leads[c][d->lead_count[c]++]=s->leads[c][i];}}}
static void score_forecast(ForecastMetrics*m,uint8_t predicted,uint8_t outcome,const Trace*t,size_t start,size_t end){m->evaluated++;for(unsigned c=0;c<3;c++){bool p=(predicted&(1U<<c))!=0,o=(outcome&(1U<<c))!=0;if(p&&o)m->tp[c]++;else if(p)m->fp[c]++;else if(o)m->fn[c]++;else m->tn[c]++;if(p&&o){for(size_t i=start;i<=end;i++)if(t->samples[i].latent_mask&(1U<<c)){if(m->lead_count[c]>=MAX_LEADS)exit(2);m->leads[c][m->lead_count[c]++]=t->samples[i].timestamp_ms-t->samples[start].timestamp_ms;break;}}}}
static ForecastResult run_forecast(const Trace*t,L1ForecastThresholds thresholds){ForecastResult out;memset(&out,0,sizeof(out));for(unsigned p=0;p<3;p++)forecast_init(&out.predictor[p]);L1Shadow shadow;l1_shadow_init(&shadow,thresholds);uint8_t pending_static=0,pending_learner=0;size_t pending_start=0;for(size_t i=0;i<t->count;i++){const Sample*s=&t->samples[i];L1ShadowStep step=l1_shadow_step_at(&shadow,s->observation,s->counters,s->timestamp_ms);if(step.feedback_ready){for(unsigned p=0;p<3;p++){uint8_t mask=p==0?0:(p==1?pending_static:pending_learner);score_forecast(&out.predictor[p],mask,step.feedback.positive_mask,t,pending_start,i);}}if(step.prediction_issued){L1Prediction fixed;if(!l1_fixed_prediction(s->observation,thresholds,&fixed))exit(2);pending_static=fixed.positive_mask;pending_learner=step.prediction.positive_mask;pending_start=i;for(unsigned p=0;p<3;p++)out.predictor[p].issued++;}for(unsigned c=0;c<3;c++){if(shadow.offset[c]<out.predictor[2].offset_min[c])out.predictor[2].offset_min[c]=shadow.offset[c];if(shadow.offset[c]>out.predictor[2].offset_max[c])out.predictor[2].offset_max[c]=shadow.offset[c];}}
    for(unsigned p=0;p<3;p++){out.predictor[p].discard_invalid=shadow.discarded_invalid;out.predictor[p].discard_gap_late=shadow.discarded_gap_or_late;out.predictor[p].discard_reset=shadow.discarded_reset;out.predictor[p].resets=shadow.reset_count;uint64_t accounted=out.predictor[p].evaluated+shadow.discarded_invalid+shadow.discarded_gap_or_late+shadow.discarded_reset;out.predictor[p].unfinished=out.predictor[p].issued>accounted?out.predictor[p].issued-accounted:0;uint64_t errors=0,total=0;for(unsigned c=0;c<3;c++){errors+=out.predictor[p].fp[c]+out.predictor[p].fn[c];total+=out.predictor[p].tp[c]+out.predictor[p].tn[c]+out.predictor[p].fp[c]+out.predictor[p].fn[c];}out.scenario_brier[p]=total?(double)errors/(double)total:1.0;}for(unsigned c=0;c<3;c++)out.predictor[2].updates[c]=shadow.updates[c];return out;}

static uint64_t action_loss(const ActionMetrics*m){return m->fp*4+m->missed[1]*16+m->missed[3]*24+m->missed[2]*40+m->switches;}
static ActionMetrics run_action(const Trace*t,const L1Policy*p,L1AdaptationMode mode){ActionMetrics m;memset(&m,0,sizeof(m));L1Core core;l1_init(&core);if(p&&!l1_apply_policy(&core,p))exit(2);if(!l1_set_adaptation_mode(&core,mode))exit(2);L1Action previous=L1_HOLD,event=L1_HOLD;size_t start=0;bool detected=false;for(size_t i=0;i<t->count;i++){const Sample*s=&t->samples[i];if(s->truth!=event){if(event!=L1_HOLD&&!detected){m.undetected[event]++;if(m.event_delay_count[event]>=MAX_DELAYS)exit(2);m.event_delays[event][m.event_delay_count[event]++]=UINT32_MAX;}event=s->truth;start=i;detected=event==L1_HOLD;if(event!=L1_HOLD)m.events[event]++;}L1Action actual=l1_step_at(&core,s->observation,s->timestamp_ms);m.samples++;if((!s->observation.valid||s->regressing)&&actual!=L1_HOLD)m.violations++;if(s->truth==L1_HOLD){if(actual!=L1_HOLD)m.fp++;}else{if(actual!=s->truth)m.missed[s->truth]++;if(!detected&&actual==s->truth){if(m.delay_count[s->truth]>=MAX_DELAYS||m.event_delay_count[s->truth]>=MAX_DELAYS)exit(2);uint32_t delay=(uint32_t)(i-start);m.delays[s->truth][m.delay_count[s->truth]++]=delay;m.event_delays[s->truth][m.event_delay_count[s->truth]++]=delay;detected=true;}}if(i&&actual!=previous)m.switches++;previous=actual;}if(event!=L1_HOLD&&!detected){m.undetected[event]++;if(m.event_delay_count[event]>=MAX_DELAYS)exit(2);m.event_delays[event][m.event_delay_count[event]++]=UINT32_MAX;}m.loss_q=action_loss(&m);return m;}
static void action_add(ActionMetrics*d,const ActionMetrics*s){d->samples+=s->samples;d->fp+=s->fp;d->switches+=s->switches;d->violations+=s->violations;d->loss_q+=s->loss_q;for(unsigned a=1;a<=3;a++){d->missed[a]+=s->missed[a];d->events[a]+=s->events[a];d->undetected[a]+=s->undetected[a];for(size_t i=0;i<s->delay_count[a];i++){if(d->delay_count[a]>=MAX_DELAYS)exit(2);d->delays[a][d->delay_count[a]++]=s->delays[a][i];}}}
static uint64_t verify_unchanged(const Trace*t,const L1Policy*p,L1ForecastThresholds ft){L1Core a,b;L1Shadow shadow;l1_init(&a);l1_init(&b);if(!l1_apply_policy(&a,p)||!l1_apply_policy(&b,p))exit(2);if(!l1_set_adaptation_mode(&a,L1_ADAPT_RISK_AWARE)||!l1_set_adaptation_mode(&b,L1_ADAPT_RISK_AWARE))exit(2);l1_shadow_init(&shadow,ft);uint64_t mismatches=0;for(size_t i=0;i<t->count;i++){const Sample*s=&t->samples[i];L1Action aa=l1_step_at(&a,s->observation,s->timestamp_ms),bb=l1_step_at(&b,s->observation,s->timestamp_ms);(void)l1_shadow_step_at(&shadow,s->observation,s->counters,s->timestamp_ms);if(aa!=bb||memcmp(&a,&b,sizeof(a)))mismatches++;}return mismatches;}

static bool better_action(const ActionMetrics*a,const L1Policy*ap,const ActionMetrics*b,const L1Policy*bp){if(a->loss_q!=b->loss_q)return a->loss_q<b->loss_q;
#define LOWER(x) if(a->x!=b->x)return a->x<b->x
    LOWER(missed[2]);LOWER(undetected[2]);LOWER(missed[3]);LOWER(undetected[3]);LOWER(missed[1]);LOWER(undetected[1]);LOWER(fp);LOWER(switches);
#undef LOWER
    if(ap->confirmation_samples!=bp->confirmation_samples)
        return ap->confirmation_samples<bp->confirmation_samples;
    if(ap->cpu_trigger_bp!=bp->cpu_trigger_bp)
        return ap->cpu_trigger_bp<bp->cpu_trigger_bp;
    if(ap->memory_trigger_bp!=bp->memory_trigger_bp)
        return ap->memory_trigger_bp<bp->memory_trigger_bp;
    return ap->io_trigger_bp<bp->io_trigger_bp;
}
static double macro_f1(const ForecastMetrics*m){double sum=0;for(unsigned c=0;c<3;c++){uint64_t den=2*m->tp[c]+m->fp[c]+m->fn[c];sum+=den?(double)(2*m->tp[c])/(double)den:0;}return sum/3.0;}
static bool better_forecast(const ForecastMetrics*a,L1ForecastThresholds at,const ForecastMetrics*b,L1ForecastThresholds bt){uint64_t ae=0,be=0,am=0,bm=0,af=0,bf=0;for(unsigned c=0;c<3;c++){ae+=a->fp[c]+a->fn[c];be+=b->fp[c]+b->fn[c];am+=a->fn[c];bm+=b->fn[c];af+=a->fp[c];bf+=b->fp[c];}if(ae!=be)return ae<be;double ax=macro_f1(a),bx=macro_f1(b);if(ax!=bx)return ax>bx;if(am!=bm)return am<bm;if(af!=bf)return af<bf;if(at.cpu_bp!=bt.cpu_bp)return at.cpu_bp<bt.cpu_bp;if(at.memory_bp!=bt.memory_bp)return at.memory_bp<bt.memory_bp;return at.io_bp<bt.io_bp;}

static bool write_manifest(const char*path,const Manifest*m){FILE*f=fopen(path,"w");if(!f)return false;int ok=fprintf(f,"protocol=l1-v0.6\ndevelopment_checksum=%016"PRIx64"\ntraining_checksum=%016"PRIx64"\naction_candidates=108\nforecast_candidates=64\naction_loss_quarters=%"PRIu64"\nforecast_errors=%"PRIu64"\nforecast_windows=%"PRIu64"\nschema_version=%u\ngeneration=%u\ncpu_trigger_bp=%u\nmemory_trigger_bp=%u\nio_trigger_bp=%u\nconfirmation_samples=%u\nforecast_cpu_bp=%u\nforecast_memory_bp=%u\nforecast_io_bp=%u\nmanifest_fingerprint=%016"PRIx64"\n",m->development_checksum,m->training_checksum,m->action_loss_q,m->forecast_errors,m->forecast_windows,m->policy.schema_version,m->policy.generation,m->policy.cpu_trigger_bp,m->policy.memory_trigger_bp,m->policy.io_trigger_bp,m->policy.confirmation_samples,m->forecast.cpu_bp,m->forecast.memory_bp,m->forecast.io_bp,m->fingerprint)>0;if(fclose(f))ok=0;return ok!=0;}
static bool parse_dec(const char*s,uint64_t*v){char*e;errno=0;unsigned long long x=strtoull(s,&e,10);if(*s<'0'||*s>'9'||errno||e==s||*e)return false;*v=(uint64_t)x;return true;}
static bool parse_hex(const char*s,uint64_t*v){char*e;errno=0;unsigned long long x=strtoull(s,&e,16);if(*s=='\0'||errno||e==s||*e)return false;*v=(uint64_t)x;return true;}
static bool read_value(FILE*f,const char*k,char*v,size_t n){char line[160];if(!fgets(line,sizeof(line),f))return false;char*nl=strchr(line,'\n');if(nl)*nl='\0';size_t z=strlen(k);if(strncmp(line,k,z)||line[z]!='='||strlen(line+z+1)>=n)return false;strcpy(v,line+z+1);return true;}
static bool read_manifest(const char*path,Manifest*m){FILE*f=fopen(path,"r");char v[80];uint64_t x=0,action_candidates=0,forecast_candidates=0;bool ok=f!=NULL;
#define READ(k) (ok=ok&&read_value(f,k,v,sizeof(v)))
    if(READ("protocol")) ok=!strcmp(v,"l1-v0.6");
    if(READ("development_checksum")) ok=parse_hex(v,&m->development_checksum);
    if(READ("training_checksum")) ok=parse_hex(v,&m->training_checksum);
    if(READ("action_candidates")) ok=parse_dec(v,&action_candidates);
    if(READ("forecast_candidates")) ok=parse_dec(v,&forecast_candidates);
    if(READ("action_loss_quarters")) ok=parse_dec(v,&m->action_loss_q);
    if(READ("forecast_errors")) ok=parse_dec(v,&m->forecast_errors);
    if(READ("forecast_windows")) ok=parse_dec(v,&m->forecast_windows);
    if(READ("schema_version")){
        ok=parse_dec(v,&x)&&x<=UINT32_MAX;
        m->policy.schema_version=(uint32_t)x;
    }
    if(READ("generation")){
        ok=parse_dec(v,&x)&&x<=UINT32_MAX;
        m->policy.generation=(uint32_t)x;
    }
    if(READ("cpu_trigger_bp")){
        ok=parse_dec(v,&x)&&x<=UINT16_MAX;
        m->policy.cpu_trigger_bp=(uint16_t)x;
    }
    if(READ("memory_trigger_bp")){
        ok=parse_dec(v,&x)&&x<=UINT16_MAX;
        m->policy.memory_trigger_bp=(uint16_t)x;
    }
    if(READ("io_trigger_bp")){
        ok=parse_dec(v,&x)&&x<=UINT16_MAX;
        m->policy.io_trigger_bp=(uint16_t)x;
    }
    if(READ("confirmation_samples")){
        ok=parse_dec(v,&x)&&x<=UINT8_MAX;
        m->policy.confirmation_samples=(uint8_t)x;
    }
    if(READ("forecast_cpu_bp")){
        ok=parse_dec(v,&x)&&x<=UINT16_MAX;
        m->forecast.cpu_bp=(uint16_t)x;
    }
    if(READ("forecast_memory_bp")){
        ok=parse_dec(v,&x)&&x<=UINT16_MAX;
        m->forecast.memory_bp=(uint16_t)x;
    }
    if(READ("forecast_io_bp")){
        ok=parse_dec(v,&x)&&x<=UINT16_MAX;
        m->forecast.io_bp=(uint16_t)x;
    }
    if(READ("manifest_fingerprint")) ok=parse_hex(v,&m->fingerprint);
#undef READ
    if(ok){int ch=fgetc(f);ok=ch==EOF&&!ferror(f);}if(f&&fclose(f))ok=false;return ok&&action_candidates==108&&forecast_candidates==64&&l1_policy_valid(&m->policy)&&l1_forecast_thresholds_valid(m->forecast)&&m->fingerprint==manifest_fingerprint(m);}

static int development(void){uint64_t checksum,invalid=0,regress=0,resets=0,corrupt=0;size_t n=sizeof(development_seeds)/sizeof(development_seeds[0]);Trace*t=generate_split(development_seeds,n,&checksum);if(!t)return 2;for(size_t i=0;i<n*FAMILY_COUNT;i++)for(size_t j=0;j<t[i].count;j++){invalid+=!t[i].samples[j].observation.valid;regress+=t[i].samples[j].regressing;resets+=t[i].samples[j].injected_reset;corrupt+=t[i].samples[j].injected_corruption;}printf("DEVELOPMENT_V06,scenarios=%zu,checksum=%016"PRIx64",invalid=%"PRIu64",regressions=%"PRIu64",resets=%"PRIu64",corrupt=%"PRIu64"\n",n*FAMILY_COUNT,checksum,invalid,regress,resets,corrupt);free_split(t,n*FAMILY_COUNT);return invalid&&regress&&resets&&corrupt?0:2;}
static int train(const char*path){static const uint16_t cv[]={2000,3000,4000},mv[]={50,100,200},iv[]={300,500,800};static const uint8_t kv[]={2,3,4,5};static const uint16_t fc[]={400,800,1200,2000},fm[]={20,40,75,125},fi[]={75,150,300,600};uint64_t checksum;size_t seeds=sizeof(training_seeds)/sizeof(training_seeds[0]),n=seeds*FAMILY_COUNT;Trace*t=generate_split(training_seeds,seeds,&checksum);if(!t)return 2;ActionMetrics best_action;memset(&best_action,0,sizeof(best_action));L1Policy bestp={0};bool have=false;for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++)for(unsigned c=0;c<3;c++)for(unsigned d=0;d<4;d++){L1Policy p={1,1,cv[a],mv[b],iv[c],kv[d]};ActionMetrics total;memset(&total,0,sizeof(total));for(size_t i=0;i<n;i++){ActionMetrics one=run_action(&t[i],&p,L1_ADAPT_DISABLED);action_add(&total,&one);}if(!have||better_action(&total,&p,&best_action,&bestp)){best_action=total;bestp=p;have=true;}}
    ForecastMetrics best_forecast;forecast_init(&best_forecast);L1ForecastThresholds bestf={0};have=false;for(unsigned a=0;a<4;a++)for(unsigned b=0;b<4;b++)for(unsigned c=0;c<4;c++){L1ForecastThresholds ft={fc[a],fm[b],fi[c]};ForecastMetrics total;forecast_init(&total);for(size_t i=0;i<n;i++){ForecastResult one=run_forecast(&t[i],ft);forecast_add(&total,&one.predictor[1]);}if(!have||better_forecast(&total,ft,&best_forecast,bestf)){best_forecast=total;bestf=ft;have=true;}}
    uint64_t errors=0;for(unsigned c=0;c<3;c++)errors+=best_forecast.fp[c]+best_forecast.fn[c];Manifest m={0};uint64_t development_checksum;size_t dev_seeds=sizeof(development_seeds)/sizeof(development_seeds[0]);Trace*dev=generate_split(development_seeds,dev_seeds,&development_checksum);free_split(dev,dev_seeds*FAMILY_COUNT);m.development_checksum=development_checksum;m.training_checksum=checksum;m.action_loss_q=best_action.loss_q;m.forecast_errors=errors;m.forecast_windows=best_forecast.evaluated;m.policy=bestp;m.forecast=bestf;m.fingerprint=manifest_fingerprint(&m);if(!write_manifest(path,&m)){free_split(t,n);return 2;}printf("TRAIN_V06,action_candidates=108,forecast_candidates=64,development_checksum=%016"PRIx64",training_checksum=%016"PRIx64",action_loss=%.2f,policy=%u|%u|%u|%u,forecast_errors=%"PRIu64",forecast_windows=%"PRIu64",forecast=%u|%u|%u,fingerprint=%016"PRIx64",manifest=%s\n",m.development_checksum,m.training_checksum,m.action_loss_q/4.0,m.policy.cpu_trigger_bp,m.policy.memory_trigger_bp,m.policy.io_trigger_bp,m.policy.confirmation_samples,m.forecast_errors,m.forecast_windows,m.forecast.cpu_bp,m.forecast.memory_bp,m.forecast.io_bp,m.fingerprint,path);free_split(t,n);return 0;}

static double ratio(uint64_t n,uint64_t d){return d?(double)n/(double)d:NAN;}
static int cmp_u64(const void*a,const void*b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return(x>y)-(x<y);}
static void lead_stats(const ForecastMetrics*m,unsigned c,double*mean,
                       uint64_t*p95,uint64_t*maximum){
    size_t n=m->lead_count[c];
    if(!n){*mean=NAN;*p95=*maximum=0;return;}
    uint64_t copy[MAX_LEADS],sum=0;
    memcpy(copy,m->leads[c],n*sizeof(*copy));
    qsort(copy,n,sizeof(*copy),cmp_u64);
    for(size_t i=0;i<n;i++)sum+=copy[i];
    *mean=(double)sum/(double)n;
    *p95=copy[((95*n+99)/100)-1];
    *maximum=copy[n-1];
}
static void print_forecast_row(const char*family,const char*name,
                               const ForecastMetrics*m){
    printf("FORECAST,%s,%s,issued=%"PRIu64",evaluated=%"PRIu64
           ",missing_invalid=%"PRIu64",missing_gap_late=%"PRIu64
           ",missing_reset=%"PRIu64",resets=%"PRIu64
           ",unfinished=%"PRIu64,
           family,name,m->issued,m->evaluated,m->discard_invalid,
           m->discard_gap_late,m->discard_reset,m->resets,m->unfinished);
    for(unsigned c=0;c<3;c++){
        uint64_t total=m->tp[c]+m->tn[c]+m->fp[c]+m->fn[c];
        double lead_mean;uint64_t lead_p95,lead_max;
        lead_stats(m,c,&lead_mean,&lead_p95,&lead_max);
        printf(",c%u_tp=%"PRIu64",tn=%"PRIu64",fp=%"PRIu64
               ",fn=%"PRIu64",precision=%.6f,recall=%.6f"
               ",specificity=%.6f,false_alarm_rate=%.6f,f1=%.6f"
               ",brier=%.6f,prevalence=%.6f,pos_cal=%.6f,neg_cal=%.6f"
               ",lead_ms=%.3f/%"PRIu64"/%"PRIu64
               ",updates=%"PRIu64",offset=%d..%d",
               c,m->tp[c],m->tn[c],m->fp[c],m->fn[c],
               ratio(m->tp[c],m->tp[c]+m->fp[c]),
               ratio(m->tp[c],m->tp[c]+m->fn[c]),
               ratio(m->tn[c],m->tn[c]+m->fp[c]),
               ratio(m->fp[c],m->tn[c]+m->fp[c]),
               ratio(2*m->tp[c],2*m->tp[c]+m->fp[c]+m->fn[c]),
               ratio(m->fp[c]+m->fn[c],total),
               ratio(m->tp[c]+m->fn[c],total),
               ratio(m->tp[c],m->tp[c]+m->fp[c]),
               ratio(m->fn[c],m->tn[c]+m->fn[c]),
               lead_mean,lead_p95,lead_max,m->updates[c],
               m->offset_min[c]==INT8_MAX?0:m->offset_min[c],
               m->offset_max[c]==INT8_MIN?0:m->offset_max[c]);
    }
    putchar('\n');
}
static int cmp_u32(const void*a,const void*b){uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b;return(x>y)-(x<y);}
static void delay_stats(const ActionMetrics*m,unsigned a,double*mean,uint32_t*p95,uint32_t*max){if(!m->delay_count[a]){*mean=0;*p95=*max=0;return;}uint32_t c[MAX_DELAYS];memcpy(c,m->delays[a],m->delay_count[a]*sizeof(*c));qsort(c,m->delay_count[a],sizeof(*c),cmp_u32);uint64_t sum=0;for(size_t i=0;i<m->delay_count[a];i++)sum+=c[i];*mean=(double)sum/m->delay_count[a];*p95=c[((95*m->delay_count[a]+99)/100)-1];*max=c[m->delay_count[a]-1];}
static void print_action_row(const char*name,const ActionMetrics*m){double cm,mm,im;uint32_t cp,cx,mp,mx,ip,ix;delay_stats(m,1,&cm,&cp,&cx);delay_stats(m,2,&mm,&mp,&mx);delay_stats(m,3,&im,&ip,&ix);printf("ACTION,%s,loss=%.2f,fp=%"PRIu64",miss_cpu=%"PRIu64",miss_io=%"PRIu64",miss_memory=%"PRIu64",undetected=%"PRIu64"/%"PRIu64"/%"PRIu64",delay_cpu=%.3f/%u/%u,delay_io=%.3f/%u/%u,delay_memory=%.3f/%u/%u,switches=%"PRIu64",fallback_violations=%"PRIu64"\n",name,m->loss_q/4.0,m->fp,m->missed[1],m->missed[3],m->missed[2],m->undetected[1],m->undetected[3],m->undetected[2],cm,cp,cx,im,ip,ix,mm,mp,mx,m->switches,m->violations);}
static long double sign_p(unsigned wins,unsigned losses){unsigned n=wins+losses;if(!n)return 1;unsigned k=wins<losses?wins:losses;long double term=ldexpl(1.0L,-(int)n),sum=term;for(unsigned i=1;i<=k;i++){term*=((long double)(n-i+1)/(long double)i);sum+=term;}long double p=2*sum;return p>1?1:p;}
static uint64_t ns_between(struct timespec a,struct timespec b){int64_t s=(int64_t)b.tv_sec-(int64_t)a.tv_sec,n=(int64_t)b.tv_nsec-(int64_t)a.tv_nsec;return(uint64_t)(s*INT64_C(1000000000)+n);}
static void pair_action_class(const ActionMetrics*fixed,const ActionMetrics*risk,
                              unsigned action,uint64_t*equal,uint64_t*faster,
                              uint64_t*slower,uint32_t*worst){
    if(fixed->event_delay_count[action]!=risk->event_delay_count[action])exit(2);
    for(size_t i=0;i<fixed->event_delay_count[action];i++){
        uint32_t f=fixed->event_delays[action][i];
        uint32_t r=risk->event_delays[action][i];
        if(r==f)(*equal)++;
        else if(r<f)(*faster)++;
        else{
            (*slower)++;
            uint32_t added=r==UINT32_MAX?UINT32_MAX:r-f;
            if(added>*worst)*worst=added;
        }
    }
}

static int heldout(const char*manifest_path,unsigned repetitions){Manifest m={0};if(!read_manifest(manifest_path,&m)){fputs("invalid V0.6 manifest\n",stderr);return 2;}if(!frozen_development_checksum||m.development_checksum!=frozen_development_checksum||m.training_checksum!=frozen_training_checksum||m.fingerprint!=frozen_manifest_fingerprint||m.action_loss_q!=frozen_action_loss_q||m.forecast_errors!=frozen_forecast_errors||m.forecast_windows!=frozen_forecast_windows){fputs("V0.6 manifest does not match frozen training identity\n",stderr);return 2;}size_t seeds=sizeof(heldout_seeds)/sizeof(heldout_seeds[0]),n=seeds*FAMILY_COUNT;uint64_t checksum;Trace*t=generate_split(heldout_seeds,seeds,&checksum);if(!t)return 2;ForecastMetrics aggregate[3];ForecastMetrics(*families)[FAMILY_COUNT]=calloc(FORECAST_COUNT,sizeof(*families));ActionMetrics actions[4];ActionMetrics(*action_families)[FAMILY_COUNT]=calloc(ACTION_COUNT,sizeof(*action_families));if(!families||!action_families){free(families);free(action_families);free_split(t,n);return 2;}for(unsigned p=0;p<3;p++){forecast_init(&aggregate[p]);for(unsigned f=0;f<FAMILY_COUNT;f++)forecast_init(&families[p][f]);}memset(actions,0,sizeof(actions));double paired[MAX_SCENARIOS];unsigned wins=0,losses=0,ties=0,seed_wins=0;uint64_t mismatches=0,memory_equal=0,memory_faster=0,memory_slower=0,io_equal=0,io_faster=0,io_slower=0;uint32_t memory_worst=0,io_worst=0;struct timespec begin,end;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&begin);for(unsigned rep=0;rep<repetitions;rep++){for(size_t i=0;i<n;i++){ForecastResult fr=run_forecast(&t[i],m.forecast);if(rep==0){for(unsigned p=0;p<3;p++){forecast_add(&aggregate[p],&fr.predictor[p]);forecast_add(&families[p][t[i].family],&fr.predictor[p]);}paired[i]=fr.scenario_brier[2]-fr.scenario_brier[1];if(paired[i]<0)wins++;else if(paired[i]>0)losses++;else ties++;for(unsigned a=0;a<3;a++){const L1Policy*p=a?&m.policy:NULL;L1AdaptationMode mode=a<2?L1_ADAPT_DISABLED:L1_ADAPT_RISK_AWARE;ActionMetrics one=run_action(&t[i],p,mode);action_add(&actions[a],&one);action_add(&action_families[a][t[i].family],&one);}ActionMetrics fixed_pair=run_action(&t[i],&m.policy,L1_ADAPT_DISABLED);ActionMetrics risk_pair=run_action(&t[i],&m.policy,L1_ADAPT_RISK_AWARE);pair_action_class(&fixed_pair,&risk_pair,L1_PROTECT_MEMORY,&memory_equal,&memory_faster,&memory_slower,&memory_worst);pair_action_class(&fixed_pair,&risk_pair,L1_DEFER_IO,&io_equal,&io_faster,&io_slower,&io_worst);mismatches+=verify_unchanged(&t[i],&m.policy,m.forecast);}}}clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&end);actions[3]=actions[2];for(unsigned f=0;f<FAMILY_COUNT;f++)action_families[3][f]=action_families[2][f];for(size_t s=0;s<seeds;s++){double sum=0;for(unsigned f=0;f<FAMILY_COUNT;f++)sum+=paired[s*FAMILY_COUNT+f];if(sum<0)seed_wins++;}
    printf("RUN_V06,checksum=%016"PRIx64",repetitions=%u,development_checksum=%016"PRIx64",training_checksum=%016"PRIx64",policy=%u|%u|%u|%u,forecast=%u|%u|%u,fingerprint=%016"PRIx64"\n",checksum,repetitions,m.development_checksum,m.training_checksum,m.policy.cpu_trigger_bp,m.policy.memory_trigger_bp,m.policy.io_trigger_bp,m.policy.confirmation_samples,m.forecast.cpu_bp,m.forecast.memory_bp,m.forecast.io_bp,m.fingerprint);struct utsname u;struct rusage usage;uname(&u);getrusage(RUSAGE_SELF,&usage);printf("ENV,compiler=%s,system=%s,machine=%s,sizeof_l1core=%zu,sizeof_l1shadow=%zu,state_bytes=72,rss_kib=%ld,cpu_ns_per_sample=%.3f\n",__VERSION__,u.sysname,u.machine,sizeof(L1Core),sizeof(L1Shadow),usage.ru_maxrss,(double)ns_between(begin,end)/(double)(n*repetitions));for(unsigned p=0;p<3;p++){for(unsigned f=0;f<FAMILY_COUNT;f++)print_forecast_row(family_names[f],forecast_names[p],&families[p][f]);print_forecast_row("ALL",forecast_names[p],&aggregate[p]);}for(unsigned a=0;a<4;a++){for(unsigned f=0;f<FAMILY_COUNT;f++)printf("ACTION_FAMILY,%s,%s,loss=%.2f\n",action_names[a],family_names[f],action_families[a][f].loss_q/4.0);print_action_row(action_names[a],&actions[a]);}
    double mean=0;for(size_t i=0;i<n;i++)mean+=paired[i];mean/=n;double var=0;for(size_t i=0;i<n;i++){double d=paired[i]-mean;var+=d*d;}var=n>1?var/(n-1):0;double sorted[MAX_SCENARIOS];memcpy(sorted,paired,n*sizeof(*sorted));for(size_t i=0;i<n;i++)for(size_t j=i+1;j<n;j++)if(sorted[j]<sorted[i]){double x=sorted[i];sorted[i]=sorted[j];sorted[j]=x;}double median=sorted[(n-1)/2];long double pvalue=sign_p(wins,losses);printf("UNCERTAINTY,paired_scenarios=%zu,mean_delta=%.9f,sd=%.9f,median=%.9f,min=%.9f,max=%.9f,wins=%u,losses=%u,ties=%u,sign_p=%.9Lf,seed_wins=%u/5\n",n,mean,sqrt(var),median,sorted[0],sorted[n-1],wins,losses,ties,pvalue,seed_wins);
    printf("PAIRED_ACTION,memory_equal=%"PRIu64",memory_faster=%"PRIu64",memory_slower=%"PRIu64",memory_worst_added=%u,io_equal=%"PRIu64",io_faster=%"PRIu64",io_slower=%"PRIu64",io_worst_added=%u\n",memory_equal,memory_faster,memory_slower,memory_worst,io_equal,io_faster,io_slower,io_worst);
    uint64_t static_errors=0,learner_errors=0;bool class_ok=true;for(unsigned c=0;c<3;c++){static_errors+=aggregate[1].fp[c]+aggregate[1].fn[c];learner_errors+=aggregate[2].fp[c]+aggregate[2].fn[c];uint64_t st=aggregate[1].tp[c]+aggregate[1].tn[c]+aggregate[1].fp[c]+aggregate[1].fn[c],lt=aggregate[2].tp[c]+aggregate[2].tn[c]+aggregate[2].fp[c]+aggregate[2].fn[c];double sb=ratio(aggregate[1].fp[c]+aggregate[1].fn[c],st),lb=ratio(aggregate[2].fp[c]+aggregate[2].fn[c],lt),sr=ratio(aggregate[1].tp[c],aggregate[1].tp[c]+aggregate[1].fn[c]),lr=ratio(aggregate[2].tp[c],aggregate[2].tp[c]+aggregate[2].fn[c]);if(lb>sb||lr+0.02<sr)class_ok=false;}double relative=static_errors?((double)static_errors-(double)learner_errors)/(double)static_errors:0;double eval_rate=aggregate[2].issued?ratio(aggregate[2].evaluated,aggregate[2].issued):0;uint64_t violations=0;for(unsigned a=0;a<4;a++)violations+=actions[a].violations;bool budgets=sizeof(L1Shadow)<=512&&usage.ru_maxrss<16384;bool go=!mismatches&&!violations&&relative>=0.05&&class_ok&&wins*5>=3*(wins+losses)&&pvalue<=0.05L&&seed_wins>=4&&eval_rate>=0.80&&budgets;printf("GUARDRAIL,action_mismatches=%"PRIu64",fallback_violations=%"PRIu64",forecast_relative_improvement=%.6f,class_guard=%s,evaluated_rate=%.6f,budgets=%s\n",mismatches,violations,relative,class_ok?"PASS":"REGRESSION",eval_rate,budgets?"PASS":"FAIL");printf("SHADOW_PREDICTION,%s\nACTION_COUPLING,NO_GO\n",go?"RESEARCH_GO":"NO_GO");free(families);free(action_families);free_split(t,n);return mismatches||violations?2:0;}
static bool repetitions(const char*s,unsigned*out){char*e;if(!s||!*s||*s=='-')return false;errno=0;unsigned long x=strtoul(s,&e,10);if(errno||*e||x<1||x>10000)return false;*out=(unsigned)x;return true;}
int main(int argc,char**argv){unsigned reps;if(argc==2&&!strcmp(argv[1],"development"))return development();if(argc==3&&!strcmp(argv[1],"train"))return train(argv[2]);if(argc==4&&!strcmp(argv[1],"heldout")&&repetitions(argv[3],&reps))return heldout(argv[2],reps);fprintf(stderr,"usage: %s development | train MANIFEST | heldout MANIFEST REPETITIONS\n",argv[0]);return 2;}
