#define _POSIX_C_SOURCE 200809L
#include "../l1_contextual_novelty.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_LINE=512, MAX_ROWS=8000, MAX_EPISODES=200, MAX_FAMILIES=12,
       NOVELTY_VALUES=10, GRID=6, K_VALUES=5, Q_VALUES=6 };
typedef enum { TRUTH_CONTEXT=0, TRUTH_NORMAL=1, TRUTH_POSITIVE=2 } Truth;
typedef enum { MODEL_NOVELTY=0, MODEL_ORIGINAL=1, MODEL_HIGH=2, MODEL_BAND=3 } Model;
typedef struct { uint16_t cpu,memory,io,k; } HighConfig;
typedef struct { uint8_t q_index; uint16_t k; uint8_t low[3],high[3]; } BandConfig;
typedef struct {
    uint64_t time; uint16_t session,row,cpu,memory,io; bool valid,eligible;
    L1PerceptionInputQuality quality; Truth truth; char episode[48],family[32];
} Row;
typedef struct { uint16_t session,start,end,declared; Truth truth; char id[48],family[32]; } Episode;
typedef struct { Row rows[MAX_ROWS]; Episode episodes[MAX_EPISODES]; size_t row_count,episode_count; uint32_t excluded[5]; } Dataset;
typedef struct { uint32_t tp,fn,tn,fp,up,un,eligible,excluded; } Metrics;
typedef struct { char name[32]; Metrics metric[4]; uint32_t episodes; } Family;
typedef struct {
    uint16_t novelty; HighConfig high[2]; uint8_t band_q[2]; uint16_t band_k[2];
    Model strongest[2]; uint64_t fingerprint;
} Lock;

static const char DATA_HEADER[] = "schema,row_id,machine,split,session_id,row_in_session,monotonic_ms,valid,quality,cpu_some_bp,memory_full_bp,io_full_bp,scoring_class,event_family,episode_id,exclude_reason\n";
static const char EP_HEADER[] = "schema,machine,split,session_id,episode_id,scoring_class,event_family,start_row,end_row,eligible_rows\n";
static const uint16_t NOVELTY[NOVELTY_VALUES]={128,256,384,512,640,768,896,960,1000,1024};
static const uint16_t CPU[GRID]={1000,2000,3000,4500,6000,8000};
static const uint16_t MEMORY[GRID]={25,50,100,200,400,800};
static const uint16_t IO[GRID]={250,500,1000,2000,3000,5000};
static const uint16_t K[K_VALUES]={1,3,5,8,12};
static const uint8_t Q[Q_VALUES]={0,1,2,5,10,20};
static const HighConfig ORIGINAL={6000,400,2000,5};

static void fail(const char *message,const char *path){fprintf(stderr,"R23B_ERROR %s: %s\n",message,path?path:"");exit(EXIT_FAILURE);}
static uint64_t decimal(const char *text,uint64_t max,const char *path){char *end=NULL;errno=0;unsigned long long v=strtoull(text,&end,10);if(errno||!text[0]||text[0]=='-'||end==text||*end||v>max||(text[0]=='0'&&text[1]))fail("invalid decimal",path);return(uint64_t)v;}
static void split(char *line,char **fields,size_t expected,const char *path){size_t n=0;char *p=line;while(n<expected){fields[n++]=p;char *c=strchr(p,',');if(!c)break;*c='\0';p=c+1;}if(n!=expected||strchr(fields[expected-1],','))fail("wrong CSV field count",path);}
static void copy_text(char *to,size_t size,const char *from,bool empty,const char *path){size_t n=strlen(from);if((!empty&&!n)||n>=size)fail("invalid text field",path);memcpy(to,from,n+1);}
static Truth truth(const char *s,const char *p){if(!strcmp(s,"context"))return TRUTH_CONTEXT;if(!strcmp(s,"normal"))return TRUTH_NORMAL;if(!strcmp(s,"positive"))return TRUTH_POSITIVE;fail("invalid truth",p);return TRUTH_CONTEXT;}
static L1PerceptionInputQuality quality(const char *s,const char *p){if(!strcmp(s,"OK"))return L1_PQ_OK;if(!strcmp(s,"ABSENT"))return L1_PQ_ABSENT;if(!strcmp(s,"INVALID"))return L1_PQ_INVALID;if(!strcmp(s,"STALE"))return L1_PQ_STALE;if(!strcmp(s,"REGRESSED"))return L1_PQ_REGRESSED;if(!strcmp(s,"RESET"))return L1_PQ_RESET;if(!strcmp(s,"UNKNOWN"))return(L1PerceptionInputQuality)99;fail("invalid quality",p);return L1_PQ_INVALID;}
static FILE *open_header(const char *path,const char *header){FILE *f=fopen(path,"rb");if(!f)fail("cannot open",path);char line[MAX_LINE];if(!fgets(line,sizeof(line),f)||strcmp(line,header))fail("wrong header",path);return f;}

static void load_rows(const char *path,char machine,const char *wanted,Dataset *data){
    memset(data,0,sizeof(*data));FILE *f=open_header(path,DATA_HEADER);char line[MAX_LINE];uint16_t previous_session=UINT16_MAX;uint64_t previous_time=0;
    while(fgets(line,sizeof(line),f)){size_t n=strlen(line);if(!n||line[n-1]!='\n'||n==sizeof(line)-1)fail("bad row line",path);line[n-1]='\0';char *x[16];split(line,x,16,path);if(data->row_count==MAX_ROWS)fail("too many rows",path);Row *r=&data->rows[data->row_count++];
        if(strcmp(x[0],"HMI_L1_R23B_CORPUS_V2")||x[2][0]!=machine||x[2][1]||strcmp(x[3],wanted))fail("row identity",path);
        r->session=(uint16_t)decimal(x[4],39,path);r->row=(uint16_t)decimal(x[5],319,path);r->time=decimal(x[6],UINT64_MAX,path);r->valid=decimal(x[7],1,path)!=0;r->quality=quality(x[8],path);
        uint64_t c=decimal(x[9],10001,path),m=decimal(x[10],10001,path),i=decimal(x[11],10001,path);r->cpu=(uint16_t)c;r->memory=(uint16_t)m;r->io=(uint16_t)i;r->truth=truth(x[12],path);copy_text(r->family,sizeof(r->family),x[13],false,path);copy_text(r->episode,sizeof(r->episode),x[14],true,path);
        bool time_ok=previous_session!=r->session||r->time>previous_time;bool range=c<=10000&&m<=10000&&i<=10000;bool reason=x[15][0]!=0;r->eligible=r->quality==L1_PQ_OK&&r->valid&&range&&time_ok&&!reason;
        if(!r->eligible){if(r->quality!=L1_PQ_OK)data->excluded[0]++;else if(!r->valid)data->excluded[1]++;else if(!range)data->excluded[2]++;else if(!time_ok)data->excluded[3]++;else data->excluded[4]++;}
        if(previous_session!=r->session){previous_session=r->session;previous_time=r->time;}else if(r->time>previous_time)previous_time=r->time;
    }if(ferror(f)||fclose(f))fail("row read failure",path);
}
static void load_episodes(const char *path,char machine,const char *wanted,Dataset *data){FILE *f=open_header(path,EP_HEADER);char line[MAX_LINE];
    while(fgets(line,sizeof(line),f)){size_t n=strlen(line);if(!n||line[n-1]!='\n')fail("bad episode line",path);line[n-1]='\0';char *x[10];split(line,x,10,path);if(data->episode_count==MAX_EPISODES)fail("too many episodes",path);Episode *e=&data->episodes[data->episode_count++];if(strcmp(x[0],"HMI_L1_R23B_CORPUS_V2")||x[1][0]!=machine||x[1][1]||strcmp(x[2],wanted))fail("episode identity",path);e->session=(uint16_t)decimal(x[3],39,path);copy_text(e->id,sizeof(e->id),x[4],false,path);e->truth=truth(x[5],path);if(e->truth==TRUTH_CONTEXT)fail("context episode",path);copy_text(e->family,sizeof(e->family),x[6],false,path);e->start=(uint16_t)decimal(x[7],319,path);e->end=(uint16_t)decimal(x[8],319,path);e->declared=(uint16_t)decimal(x[9],320,path);if(e->start>e->end)fail("episode range",path);
        uint32_t actual=0;for(size_t j=0;j<data->row_count;j++){Row *r=&data->rows[j];if(r->session==e->session&&r->row>=e->start&&r->row<=e->end){if(strcmp(r->episode,e->id)||strcmp(r->family,e->family)||r->truth!=e->truth)fail("row/episode mismatch",path);if(r->eligible)actual++;}}
        if(actual!=e->declared)fail("post-exclusion eligibility mismatch",path);
    }if(ferror(f)||fclose(f))fail("episode read failure",path);
}
static void paths(char *out,size_t size,const char *root,char machine,const char *split,bool episodes){int n=snprintf(out,size,"%s/machine_%c_%s%s.csv",root,(char)(machine-'A'+'a'),split,episodes?"_episodes":"");if(n<0||(size_t)n>=size)fail("path too long",root);}
static uint8_t quantize(uint16_t value,uint32_t t){uint32_t q=(t+3)/4,h=(t+1)/2,v=value;if(!v)return 0;if(v<q)return 1;if(v<h)return 2;if(v<t)return 3;if(v<t*2)return 4;if(v<t*4)return 5;if(v<t*8)return 6;return 7;}
static uint64_t fnv(const uint8_t *p,size_t n){uint64_t h=UINT64_C(1469598103934665603);for(size_t i=0;i<n;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;}

static void train(const char *root,char machine,L1OperationalMemory *memory,L1OperationalMemorySummary *summary,BandConfig bands[Q_VALUES],uint64_t *checkpoint){char path[512];Dataset data;paths(path,sizeof(path),root,machine,"train",false);load_rows(path,machine,"train",&data);uint32_t normal[3][8]={{0}};l1_operational_memory_init(memory);
    for(size_t i=0;i<data.row_count;i++){Row *r=&data.rows[i];L1Observation o={r->cpu,r->memory,r->io,r->valid};(void)l1_operational_memory_step(memory,o,r->quality,r->time);if(r->eligible&&r->truth==TRUTH_NORMAL){normal[0][quantize(r->cpu,3000)]++;normal[1][quantize(r->memory,100)]++;normal[2][quantize(r->io,500)]++;}}
    *summary=l1_operational_memory_summary(memory);if(summary->status!=L1_OM_SUPPORTED)fail("unsupported training reference",path);uint8_t bytes[L1_OM_CHECKPOINT_BYTES];if(!l1_operational_memory_encode(memory,bytes,sizeof(bytes)))fail("checkpoint encode",path);*checkpoint=fnv(bytes,sizeof(bytes));
    for(size_t qi=0;qi<Q_VALUES;qi++){bands[qi].q_index=(uint8_t)qi;for(size_t c=0;c<3;c++){uint32_t total=0;for(size_t b=0;b<8;b++)total+=normal[c][b];uint32_t target=(total*Q[qi])/100U;uint32_t sum=0;uint8_t low=0,high=7;if(Q[qi]!=0){while(low<7&&sum+normal[c][low]<target){sum+=normal[c][low];low++;}sum=0;while(high>0&&sum+normal[c][high]<target){sum+=normal[c][high];high--;}}bands[qi].low[c]=low;bands[qi].high[c]=high;}}
}

static bool high_hit(const Row *r,HighConfig c){return r->cpu>=c.cpu||r->memory>=c.memory||r->io>=c.io;}
static bool band_hit(const Row *r,const BandConfig *c){uint8_t b[3]={quantize(r->cpu,3000),quantize(r->memory,100),quantize(r->io,500)};for(size_t i=0;i<3;i++)if(b[i]<c->low[i]||b[i]>c->high[i])return true;return false;}
static int episode_decision(const Dataset *d,const Episode *e,Model model,const L1OperationalMemorySummary *ref,uint16_t threshold,HighConfig high,const BandConfig *band,uint32_t *eligible,uint32_t *hits){*eligible=0;*hits=0;for(size_t i=0;i<d->row_count;i++){const Row *r=&d->rows[i];if(r->session!=e->session||r->row<e->start||r->row>e->end||!r->eligible)continue;(*eligible)++;bool hit=false;if(model==MODEL_NOVELTY){L1ContextualNoveltyResult n=l1_contextual_novelty_score(ref,(L1Observation){r->cpu,r->memory,r->io,true},L1_PQ_OK,threshold);if(n.disposition==L1_CONTEXTUAL_NOVELTY_UNKNOWN)return-1;hit=n.disposition==L1_CONTEXTUAL_NOVELTY_SUSPECTED;}else if(model==MODEL_BAND)hit=band_hit(r,band);else hit=high_hit(r,high);if(hit)(*hits)++;}if(*eligible<10)return-1;uint16_t k=model==MODEL_BAND?band->k:high.k;return *hits>=k;}
static Metrics evaluate(const Dataset *d,Model model,const L1OperationalMemorySummary *ref,uint16_t threshold,HighConfig high,const BandConfig *band){Metrics m={0};for(size_t i=0;i<d->episode_count;i++){uint32_t eligible,hits;int decision=episode_decision(d,&d->episodes[i],model,ref,threshold,high,band,&eligible,&hits);m.eligible+=eligible;m.excluded+=(uint32_t)(d->episodes[i].end-d->episodes[i].start+1)-eligible;if(decision<0){if(d->episodes[i].truth==TRUTH_POSITIVE)m.up++;else m.un++;}else if(d->episodes[i].truth==TRUTH_POSITIVE){if(decision)m.tp++;else m.fn++;}else{if(decision)m.fp++;else m.tn++;}}return m;}
static uint32_t ppm(uint32_t n,uint32_t d){return d?(uint32_t)((uint64_t)n*1000000U/d):0;}
static bool cap(Metrics m){uint32_t n=m.fp+m.tn,p=m.tp+m.fn;return n>=10&&p>=10&&(uint64_t)m.fp*10U<=n;}
static bool metric_better(Metrics m,Metrics best,bool have){if(!have)return true;uint32_t s=ppm(m.tp,m.tp+m.fn),bs=ppm(best.tp,best.tp+best.fn);if(s!=bs)return s>bs;uint32_t f=ppm(m.fp,m.fp+m.tn),bf=ppm(best.fp,best.fp+best.tn);return f<bf;}
static bool high_lex(HighConfig a,HighConfig b){if(a.cpu!=b.cpu)return a.cpu<b.cpu;if(a.memory!=b.memory)return a.memory<b.memory;if(a.io!=b.io)return a.io<b.io;return a.k<b.k;}
static HighConfig tune_high(const Dataset *d,Metrics *out){HighConfig best={0};Metrics bm={0};bool have=false;for(size_t c=0;c<GRID;c++)for(size_t m=0;m<GRID;m++)for(size_t i=0;i<GRID;i++)for(size_t k=0;k<K_VALUES;k++){HighConfig x={CPU[c],MEMORY[m],IO[i],K[k]};Metrics v=evaluate(d,MODEL_HIGH,NULL,0,x,NULL);if(cap(v)&&(metric_better(v,bm,have)||(!metric_better(bm,v,true)&&!metric_better(v,bm,true)&&high_lex(x,best)))){best=x;bm=v;have=true;}}if(!have)fail("no feasible high comparator",NULL);*out=bm;return best;}
static BandConfig tune_band(const Dataset *d,const BandConfig trained[Q_VALUES],Metrics *out){BandConfig best={0};Metrics bm={0};bool have=false;for(size_t q=0;q<Q_VALUES;q++)for(size_t k=0;k<K_VALUES;k++){BandConfig x=trained[q];x.k=K[k];Metrics v=evaluate(d,MODEL_BAND,NULL,0,(HighConfig){0},&x);if(cap(v)&&(metric_better(v,bm,have)||(!metric_better(bm,v,true)&&!metric_better(v,bm,true)&&(Q[x.q_index]<Q[best.q_index]||(x.q_index==best.q_index&&x.k<best.k))))){best=x;bm=v;have=true;}}if(!have)fail("no feasible band comparator",NULL);*out=bm;return best;}
static const char *model_name(Model m){return m==MODEL_NOVELTY?"novelty":m==MODEL_ORIGINAL?"fixed-original":m==MODEL_HIGH?"fixed-high":m==MODEL_BAND?"fixed-band":"invalid";}
static void print_metric(const char *phase,char machine,const char *name,Metrics m){printf("METRIC phase=%s machine=%c model=%s tp=%"PRIu32" fn=%"PRIu32" tn=%"PRIu32" fp=%"PRIu32" unknown_positive=%"PRIu32" unknown_normal=%"PRIu32" sensitivity_ppm=%"PRIu32" false_alert_ppm=%"PRIu32" eligible_rows=%"PRIu32" excluded_rows=%"PRIu32"\n",phase,machine,name,m.tp,m.fn,m.tn,m.fp,m.up,m.un,ppm(m.tp,m.tp+m.fn),ppm(m.fp,m.fp+m.tn),m.eligible,m.excluded);}
static Model strongest(Metrics original,Metrics high,Metrics band){Model best=cap(original)?MODEL_ORIGINAL:MODEL_HIGH;Metrics value=cap(original)?original:high;if(metric_better(high,value,true)||(!metric_better(value,high,true)&&!metric_better(high,value,true)))best=MODEL_HIGH,value=high;if(metric_better(band,value,true))best=MODEL_BAND;return best;}
static bool novelty_better(Metrics a,Metrics b,uint16_t t,Metrics ba,Metrics bb,uint16_t bt,bool have){if(!have)return true;uint32_t min=ppm(a.tp,a.tp+a.fn),v=ppm(b.tp,b.tp+b.fn);if(v<min)min=v;uint32_t bmin=ppm(ba.tp,ba.tp+ba.fn);v=ppm(bb.tp,bb.tp+bb.fn);if(v<bmin)bmin=v;if(min!=bmin)return min>bmin;uint32_t sum=ppm(a.tp,a.tp+a.fn)+ppm(b.tp,b.tp+b.fn),bs=ppm(ba.tp,ba.tp+ba.fn)+ppm(bb.tp,bb.tp+bb.fn);if(sum!=bs)return sum>bs;uint32_t max=ppm(a.fp,a.fp+a.tn),x=ppm(b.fp,b.fp+b.tn);if(x>max)max=x;uint32_t bmax=ppm(ba.fp,ba.fp+ba.tn);x=ppm(bb.fp,bb.fp+bb.tn);if(x>bmax)bmax=x;if(max!=bmax)return max<bmax;sum=ppm(a.fp,a.fp+a.tn)+ppm(b.fp,b.fp+b.tn);bs=ppm(ba.fp,ba.fp+ba.tn)+ppm(bb.fp,bb.fp+bb.tn);if(sum!=bs)return sum<bs;return t>bt;}

static void calibrate(const char *root){L1OperationalMemory mem[2];L1OperationalMemorySummary ref[2];BandConfig trained[2][Q_VALUES],selected_band[2];Dataset data[2];uint64_t checkpoint[2];HighConfig selected_high[2];Metrics high_m[2],band_m[2],original_m[2],novelty_m[2]={{0}};Model strong[2];char path[512];
    for(size_t x=0;x<2;x++){char machine=(char)('A'+x);train(root,machine,&mem[x],&ref[x],trained[x],&checkpoint[x]);paths(path,sizeof(path),root,machine,"calibration",false);load_rows(path,machine,"calibration",&data[x]);paths(path,sizeof(path),root,machine,"calibration",true);load_episodes(path,machine,"calibration",&data[x]);selected_high[x]=tune_high(&data[x],&high_m[x]);selected_band[x]=tune_band(&data[x],trained[x],&band_m[x]);original_m[x]=evaluate(&data[x],MODEL_ORIGINAL,NULL,0,ORIGINAL,NULL);strong[x]=strongest(original_m[x],high_m[x],band_m[x]);}
    bool have=false;uint16_t threshold=1024;for(size_t i=0;i<NOVELTY_VALUES;i++){Metrics a=evaluate(&data[0],MODEL_NOVELTY,&ref[0],NOVELTY[i],(HighConfig){0},NULL),b=evaluate(&data[1],MODEL_NOVELTY,&ref[1],NOVELTY[i],(HighConfig){0},NULL);if(cap(a)&&cap(b)&&novelty_better(a,b,NOVELTY[i],novelty_m[0],novelty_m[1],threshold,have)){novelty_m[0]=a;novelty_m[1]=b;threshold=NOVELTY[i];have=true;}}
    if(!have){threshold=1024;for(size_t x=0;x<2;x++)novelty_m[x]=evaluate(&data[x],MODEL_NOVELTY,&ref[x],threshold,(HighConfig){0},NULL);}
    for(size_t x=0;x<2;x++){char machine=(char)('A'+x);print_metric("calibration",machine,"novelty",novelty_m[x]);print_metric("calibration",machine,"fixed-original",original_m[x]);print_metric("calibration",machine,"fixed-high",high_m[x]);print_metric("calibration",machine,"fixed-band",band_m[x]);printf("CALIBRATION_MACHINE machine=%c high=%u|%u|%u|%u band=%u|%u band_bins=%u-%u|%u-%u|%u-%u strongest=%s checkpoint=%016"PRIx64" exclusions=%"PRIu32"|%"PRIu32"|%"PRIu32"|%"PRIu32"|%"PRIu32"\n",machine,selected_high[x].cpu,selected_high[x].memory,selected_high[x].io,selected_high[x].k,Q[selected_band[x].q_index],selected_band[x].k,selected_band[x].low[0],selected_band[x].high[0],selected_band[x].low[1],selected_band[x].high[1],selected_band[x].low[2],selected_band[x].high[2],model_name(strong[x]),checkpoint[x],data[x].excluded[0],data[x].excluded[1],data[x].excluded[2],data[x].excluded[3],data[x].excluded[4]);}
    printf("CALIBRATION_LOCK_VALUES novelty_threshold=%u novelty_cap=%s\n",threshold,have?"PASS":"CALIBRATION_CAP_NOT_MET");
}

static uint64_t fnv_line(uint64_t h,const char *s,size_t n){for(size_t i=0;i<n;i++){h^=(uint8_t)s[i];h*=UINT64_C(1099511628211);}return h;}
static bool member(const uint16_t *values,size_t count,uint16_t value){for(size_t i=0;i<count;i++)if(values[i]==value)return true;return false;}
static Lock read_lock(const char *path){Lock l={0};bool schema=false,threshold=false,af=false,bf=false,ab=false,bb=false,as=false,bs=false,corpus=false,finger=false;FILE *f=fopen(path,"rb");if(!f)fail("cannot open lock",path);char line[MAX_LINE];uint64_t hash=UINT64_C(1469598103934665603);
    while(fgets(line,sizeof(line),f)){size_t n=strlen(line);if(!n||line[n-1]!='\n')fail("bad lock line",path);if(!strncmp(line,"lock_fingerprint=",17)){char *end=NULL;errno=0;l.fingerprint=strtoull(line+17,&end,16);if(errno||end!=line+n-1)fail("bad fingerprint",path);finger=true;continue;}if(finger)fail("fingerprint not final",path);hash=fnv_line(hash,line,n);line[n-1]='\0';char *eq=strchr(line,'=');if(!eq||strchr(eq+1,'='))fail("bad lock field",path);*eq='\0';const char *v=eq+1;
        if(!strcmp(line,"schema")){if(schema||strcmp(v,"HMI_L1_R23B_CALIBRATION_LOCK_V1"))fail("lock schema",path);schema=true;}
        else if(!strcmp(line,"novelty_threshold")){l.novelty=(uint16_t)decimal(v,1024,path);threshold=true;}
        else if(!strcmp(line,"machine_a_high")||!strcmp(line,"machine_b_high")){size_t x=line[8]=='a'?0:1;unsigned c,m,i,k;char tail;if(sscanf(v,"%u|%u|%u|%u%c",&c,&m,&i,&k,&tail)!=4)fail("lock high",path);l.high[x]=(HighConfig){(uint16_t)c,(uint16_t)m,(uint16_t)i,(uint16_t)k};if(x)bf=true;else af=true;}
        else if(!strcmp(line,"machine_a_band")||!strcmp(line,"machine_b_band")){size_t x=line[8]=='a'?0:1;unsigned q,k;char tail;if(sscanf(v,"%u|%u%c",&q,&k,&tail)!=2||k>UINT16_MAX)fail("lock band",path);size_t qi=0;while(qi<Q_VALUES&&Q[qi]!=q)qi++;if(qi==Q_VALUES)fail("lock band q",path);l.band_q[x]=(uint8_t)qi;l.band_k[x]=(uint16_t)k;if(x)bb=true;else ab=true;}
        else if(!strcmp(line,"machine_a_strongest")||!strcmp(line,"machine_b_strongest")){size_t x=line[8]=='a'?0:1;if(!strcmp(v,"fixed-original"))l.strongest[x]=MODEL_ORIGINAL;else if(!strcmp(v,"fixed-high"))l.strongest[x]=MODEL_HIGH;else if(!strcmp(v,"fixed-band"))l.strongest[x]=MODEL_BAND;else fail("lock strongest",path);if(x)bs=true;else as=true;}
        else if(!strcmp(line,"corpus_sha256")){if(strcmp(v,"c4db4c939522b58841da43224d80a1372a7f0a91b60a88dabb2b706f1735a26b"))fail("wrong corpus identity",path);corpus=true;}
        else if(!strncmp(line,"identity_",9)||!strncmp(line,"calibration_",12)||!strncmp(line,"checkpoint_",11)||!strcmp(line,"generator_sha256")||!strcmp(line,"selection_version")||!strcmp(line,"novelty_cap")){if(!*v)fail("empty lock evidence",path);}
        else fail("unknown lock field",path);
    }if(ferror(f)||fclose(f)||!schema||!threshold||!af||!bf||!ab||!bb||!as||!bs||!corpus||!finger||hash!=l.fingerprint)fail("incomplete lock",path);bool ok=false;for(size_t i=0;i<NOVELTY_VALUES;i++)if(l.novelty==NOVELTY[i])ok=true;if(!ok||!member(CPU,GRID,l.high[0].cpu)||!member(CPU,GRID,l.high[1].cpu)||!member(MEMORY,GRID,l.high[0].memory)||!member(MEMORY,GRID,l.high[1].memory)||!member(IO,GRID,l.high[0].io)||!member(IO,GRID,l.high[1].io)||!member(K,K_VALUES,l.high[0].k)||!member(K,K_VALUES,l.high[1].k)||!member(K,K_VALUES,l.band_k[0])||!member(K,K_VALUES,l.band_k[1]))fail("lock configuration outside frozen sets",path);return l;}
static size_t family_index(Family f[MAX_FAMILIES],size_t *count,const char *name){for(size_t i=0;i<*count;i++)if(!strcmp(f[i].name,name))return i;if(*count==MAX_FAMILIES)fail("too many families",name);copy_text(f[*count].name,sizeof(f[*count].name),name,false,name);return(*count)++;}
static Metrics select_metric(Metrics values[4],Model m){return values[(size_t)m];}
typedef struct {bool support,noninferior,family_win;} Gate;
static Gate heldout_machine(const char *root,const Lock *lock,size_t x){char machine=(char)('A'+x),path[512];L1OperationalMemory mem;L1OperationalMemorySummary ref;BandConfig trained[Q_VALUES];uint64_t checkpoint;train(root,machine,&mem,&ref,trained,&checkpoint);BandConfig band=trained[lock->band_q[x]];band.k=lock->band_k[x];Dataset d;paths(path,sizeof(path),root,machine,"heldout",false);load_rows(path,machine,"heldout",&d);paths(path,sizeof(path),root,machine,"heldout",true);load_episodes(path,machine,"heldout",&d);Metrics total[4]={{0}};Family families[MAX_FAMILIES];memset(families,0,sizeof(families));size_t family_count=0;const Model models[4]={MODEL_NOVELTY,MODEL_ORIGINAL,MODEL_HIGH,MODEL_BAND};
    for(size_t e=0;e<d.episode_count;e++){size_t fi=family_index(families,&family_count,d.episodes[e].family);families[fi].episodes++;printf("EPISODE machine=%c id=%s family=%s truth=%s",machine,d.episodes[e].id,d.episodes[e].family,d.episodes[e].truth==TRUTH_POSITIVE?"positive":"normal");for(size_t mi=0;mi<4;mi++){uint32_t eligible,hits;HighConfig hc=models[mi]==MODEL_ORIGINAL?ORIGINAL:lock->high[x];int decision=episode_decision(&d,&d.episodes[e],models[mi],&ref,lock->novelty,hc,&band,&eligible,&hits);Metrics *targets[2]={&total[mi],&families[fi].metric[mi]};for(size_t t=0;t<2;t++){targets[t]->eligible+=eligible;targets[t]->excluded+=(uint32_t)(d.episodes[e].end-d.episodes[e].start+1)-eligible;if(decision<0){if(d.episodes[e].truth==TRUTH_POSITIVE)targets[t]->up++;else targets[t]->un++;}else if(d.episodes[e].truth==TRUTH_POSITIVE){if(decision)targets[t]->tp++;else targets[t]->fn++;}else{if(decision)targets[t]->fp++;else targets[t]->tn++;}}printf(" %s=%s/%"PRIu32,model_name(models[mi]),decision<0?"UNKNOWN":decision?"SUSPECTED":"EXPECTED",hits);}puts("");}
    for(size_t i=0;i<4;i++){
        print_metric("heldout",machine,model_name(models[i]),total[i]);
    }
    for(size_t f=0;f<family_count;f++){
        for(size_t i=0;i<4;i++){
            printf("FAMILY name=%s episodes=%"PRIu32" ",families[f].name,families[f].episodes);
            print_metric("heldout",machine,model_name(models[i]),families[f].metric[i]);
        }
    }
    Metrics learned=total[0],fixed=select_metric(total,lock->strongest[x]);bool support=learned.tp+learned.fn>=20&&learned.fp+learned.tn>=20&&!learned.up&&!learned.un;bool non=learned.fp<=fixed.fp&&learned.tp>=fixed.tp;bool win=false;for(size_t f=0;f<family_count;f++){Metrics a=families[f].metric[0],b=families[f].metric[lock->strongest[x]];if(a.tp+a.fn>=5&&a.tp>b.tp)win=true;}printf("MACHINE_GATE machine=%c support=%s strongest=%s noninferior=%s family_win=%s checkpoint=%016"PRIx64"\n",machine,support?"PASS":"FAIL",model_name(lock->strongest[x]),non?"PASS":"FAIL",win?"PASS":"FAIL",checkpoint);return(Gate){support,non,win};}
static void heldout(const char *root,const char *lock_path){Lock l=read_lock(lock_path);Gate a=heldout_machine(root,&l,0),b=heldout_machine(root,&l,1);const char *u=(!a.support||!b.support)?"INSUFFICIENT_SUPPORT":(a.noninferior&&b.noninferior&&(a.family_win||b.family_win))?"GO":"NO_GO";printf("R23B_HELDOUT_COMPLETE no_retuning=YES R2_3B_STUDY=VALID R2_UTILITY=%s\n",u);}

static void probes(const char *root,const char *lock_path){Lock l=read_lock(lock_path);L1OperationalMemory mem[2];L1OperationalMemorySummary ref[2];BandConfig bands[Q_VALUES];uint64_t check;for(size_t x=0;x<2;x++)train(root,(char)('A'+x),&mem[x],&ref[x],bands,&check);char path[512];snprintf(path,sizeof(path),"%s/contextual_probes.csv",root);FILE *f=fopen(path,"rb");if(!f)fail("cannot open probes",path);char line[MAX_LINE];if(!fgets(line,sizeof(line),f))fail("probe header",path);char current[32]="";uint32_t hits[2]={0},eligible=0;
    while(fgets(line,sizeof(line),f)){size_t n=strlen(line);if(!n||line[n-1]!='\n')fail("probe row",path);line[n-1]='\0';char *xv[10];split(line,xv,10,path);if(strcmp(xv[0],"HMI_L1_R23B_CORPUS_V2"))fail("probe schema",path);if(strcmp(current,xv[1])){if(current[0]){for(size_t m=0;m<2;m++){const char *decision=eligible<10?"UNKNOWN":hits[m]>=5?"SUSPECTED":"EXPECTED";printf("PROBE id=%s machine=%c decision=%s hits=%"PRIu32" eligible=%"PRIu32"\n",current,(char)('A'+m),decision,hits[m],eligible);} }copy_text(current,sizeof(current),xv[1],false,path);hits[0]=hits[1]=eligible=0;}
        bool valid=decimal(xv[4],1,path)!=0;L1PerceptionInputQuality q=quality(xv[5],path);uint16_t cpu=(uint16_t)decimal(xv[6],10001,path),memory=(uint16_t)decimal(xv[7],10001,path),io=(uint16_t)decimal(xv[8],10001,path);if(q==L1_PQ_OK&&valid&&cpu<=10000&&memory<=10000&&io<=10000&&!xv[9][0])eligible++;for(size_t m=0;m<2;m++){L1ContextualNoveltyResult r=l1_contextual_novelty_score(&ref[m],(L1Observation){cpu,memory,io,valid},q,l.novelty);if(r.disposition==L1_CONTEXTUAL_NOVELTY_SUSPECTED)hits[m]++;}}
    if(current[0]){
        for(size_t m=0;m<2;m++){
            const char *decision=eligible<10?"UNKNOWN":hits[m]>=5?"SUSPECTED":"EXPECTED";
            printf("PROBE id=%s machine=%c decision=%s hits=%"PRIu32" eligible=%"PRIu32"\n",current,(char)('A'+m),decision,hits[m],eligible);
        }
    }
    if(ferror(f)||fclose(f))fail("probe read",path);
    puts("R23B_PROBES_COMPLETE frozen_truth=external actions_unchanged=YES");
}
int main(int argc,char **argv){if(argc==3&&!strcmp(argv[1],"calibrate")){calibrate(argv[2]);return EXIT_SUCCESS;}if(argc==4&&!strcmp(argv[1],"heldout")){heldout(argv[2],argv[3]);return EXIT_SUCCESS;}if(argc==4&&!strcmp(argv[1],"probes")){probes(argv[2],argv[3]);return EXIT_SUCCESS;}fprintf(stderr,"usage: %s calibrate CORPUS_DIR | heldout CORPUS_DIR LOCK | probes CORPUS_DIR LOCK\n",argv[0]);return EXIT_FAILURE;}
