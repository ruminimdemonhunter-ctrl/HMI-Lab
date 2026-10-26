#define _POSIX_C_SOURCE 200809L
#include "../l1_operational_memory.h"
#include "../l1_shadow.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"R23C_GATE_A_FAIL line=%d check=%s\n",__LINE__,#c); exit(EXIT_FAILURE); } } while (0)
enum { MAX_LINE=512, CSV_FIELDS=16 };

static L1Observation sample(void){return(L1Observation){1200,40,180,true};}
static uint64_t fnv64(const uint8_t *data,size_t length){uint64_t hash=UINT64_C(14695981039346656037);for(size_t i=0;i<length;i++){hash^=data[i];hash*=UINT64_C(1099511628211);}return hash;}
static void accepted_bucket(L1OperationalMemory *m,uint64_t base,size_t count){for(size_t i=0;i<count;i++)CHECK(l1_operational_memory_step(m,sample(),L1_PQ_OK,base+i*1000U)!=L1_OM_RESET);}
static void four_buckets(L1OperationalMemory *m,uint64_t base,size_t last){accepted_bucket(m,base,64);accepted_bucket(m,base+300000,64);accepted_bucket(m,base+600000,64);accepted_bucket(m,base+900000,last);}
static void three_buckets_256(L1OperationalMemory *m,uint64_t base){accepted_bucket(m,base,86);accepted_bucket(m,base+300000,85);accepted_bucket(m,base+600000,85);}

static void known_support_cases(void){
    L1OperationalMemory m;l1_operational_memory_init(&m);CHECK(m.status==L1_OM_WARMING_UP&&m.accepted_lifetime==0);
    three_buckets_256(&m,0);CHECK(m.status==L1_OM_WARMING_UP&&m.accepted_lifetime==256&&m.windows_seen==3);
    l1_operational_memory_init(&m);four_buckets(&m,0,63);CHECK(m.status==L1_OM_WARMING_UP&&m.accepted_lifetime==255&&m.windows_seen==4);
    l1_operational_memory_init(&m);four_buckets(&m,0,64);CHECK(m.status==L1_OM_SUPPORTED&&m.accepted_lifetime==256&&m.windows_seen==4);uint8_t checkpoint[L1_OM_CHECKPOINT_BYTES];CHECK(l1_operational_memory_encode(&m,checkpoint,sizeof(checkpoint)));printf("R23C_GATE_A_P1 checkpoint_fnv64=%016"PRIx64" bytes=%d\n",fnv64(checkpoint,sizeof(checkpoint)),L1_OM_CHECKPOINT_BYTES);
    const L1PerceptionInputQuality resets[2]={L1_PQ_RESET,L1_PQ_REGRESSED};
    for(size_t r=0;r<2;r++){CHECK(l1_operational_memory_step(&m,sample(),resets[r],1200000)==L1_OM_RESET);CHECK(m.accepted_lifetime==0&&m.windows_seen==0);three_buckets_256(&m,1800000);CHECK(m.status==L1_OM_WARMING_UP&&m.windows_seen==3);four_buckets(&m,3000000,64);CHECK(m.status==L1_OM_SUPPORTED&&m.windows_seen>=4);}
    l1_operational_memory_init(&m);four_buckets(&m,0,64);CHECK(l1_operational_memory_step(&m,sample(),L1_PQ_OK,900000)==L1_OM_RESET);CHECK(m.accepted_lifetime==0);three_buckets_256(&m,1800000);CHECK(m.status==L1_OM_WARMING_UP&&m.windows_seen==3);four_buckets(&m,3000000,64);CHECK(m.status==L1_OM_SUPPORTED);
}

static void invalid_and_restore_cases(void){
    L1OperationalMemory m;l1_operational_memory_init(&m);four_buckets(&m,0,64);uint32_t accepted=m.accepted_lifetime;uint16_t windows=m.windows_seen;
    const L1PerceptionInputQuality invalids[]={L1_PQ_INVALID,L1_PQ_STALE,L1_PQ_ABSENT,(L1PerceptionInputQuality)99};
    for(size_t i=0;i<sizeof(invalids)/sizeof(invalids[0]);i++){CHECK(l1_operational_memory_step(&m,sample(),invalids[i],1000000+i)==L1_OM_INVALID);CHECK(m.accepted_lifetime==accepted&&m.windows_seen==windows);}
    CHECK(l1_operational_memory_step(&m,(L1Observation){10001,0,0,true},L1_PQ_OK,1001000)==L1_OM_INVALID);CHECK(m.accepted_lifetime==accepted);
    CHECK(l1_operational_memory_step(&m,(L1Observation){0,0,0,false},L1_PQ_OK,1001000)==L1_OM_INVALID);CHECK(m.accepted_lifetime==accepted);
    CHECK(l1_operational_memory_step(&m,sample(),L1_PQ_OK,1001000)==L1_OM_SUPPORTED);
    uint8_t encoded[L1_OM_CHECKPOINT_BYTES];CHECK(l1_operational_memory_encode(&m,encoded,sizeof(encoded)));uint8_t corrupt[L1_OM_CHECKPOINT_BYTES];memcpy(corrupt,encoded,sizeof(corrupt));corrupt[20]^=1U;L1OperationalMemory restored;CHECK(!l1_operational_memory_decode(&restored,corrupt,sizeof(corrupt)));CHECK(restored.status==L1_OM_WARMING_UP&&restored.accepted_lifetime==0);
    CHECK(l1_operational_memory_decode(&restored,encoded,sizeof(encoded)));CHECK(restored.status==L1_OM_RESTORED_UNVERIFIED);accepted_bucket(&restored,3000000,64);accepted_bucket(&restored,3300000,63);CHECK(restored.status==L1_OM_RESTORED_UNVERIFIED&&restored.refresh_accepted==127&&restored.refresh_windows==2);accepted_bucket(&restored,3300000+63000,1);CHECK(restored.status==L1_OM_SUPPORTED&&restored.refresh_accepted==128);
}

static void split_csv(char *line,char **fields,const char *path){size_t count=0;char *p=line;while(count<CSV_FIELDS){fields[count++]=p;char *comma=strchr(p,',');if(!comma)break;*comma='\0';p=comma+1;}if(count!=CSV_FIELDS||strchr(fields[CSV_FIELDS-1],',')){fprintf(stderr,"R23C_GATE_A_FAIL malformed_csv=%s\n",path);exit(EXIT_FAILURE);}}
static uint64_t number(const char *s,uint64_t maximum,const char *path){char *end=NULL;errno=0;unsigned long long value=strtoull(s,&end,10);if(errno||!s[0]||s[0]=='-'||end==s||*end||value>maximum){fprintf(stderr,"R23C_GATE_A_FAIL decimal=%s\n",path);exit(EXIT_FAILURE);}return(uint64_t)value;}
static L1PerceptionInputQuality parse_quality(const char *s,const char *path){if(!strcmp(s,"OK"))return L1_PQ_OK;if(!strcmp(s,"ABSENT"))return L1_PQ_ABSENT;if(!strcmp(s,"INVALID"))return L1_PQ_INVALID;if(!strcmp(s,"STALE"))return L1_PQ_STALE;if(!strcmp(s,"REGRESSED"))return L1_PQ_REGRESSED;if(!strcmp(s,"RESET"))return L1_PQ_RESET;if(!strcmp(s,"UNKNOWN"))return(L1PerceptionInputQuality)99;fprintf(stderr,"R23C_GATE_A_FAIL quality=%s\n",path);exit(EXIT_FAILURE);}
static void historical_negative(const char *path){
    static const char header[]="schema,row_id,machine,split,session_id,row_in_session,monotonic_ms,valid,quality,cpu_some_bp,memory_full_bp,io_full_bp,scoring_class,event_family,episode_id,exclude_reason\n";
    FILE *f=fopen(path,"rb");if(!f){perror(path);exit(EXIT_FAILURE);}char line[MAX_LINE];CHECK(fgets(line,sizeof(line),f)!=NULL&&strcmp(line,header)==0);L1OperationalMemory m;l1_operational_memory_init(&m);uint32_t rows=0,last_session=UINT32_MAX,last_row=UINT32_MAX;const char *cause="NONE";
    while(fgets(line,sizeof(line),f)){size_t n=strlen(line);CHECK(n>0&&line[n-1]=='\n'&&n<sizeof(line)-1);line[n-1]='\0';char *x[CSV_FIELDS];split_csv(line,x,path);CHECK(!strcmp(x[0],"HMI_L1_R23B_CORPUS_V2")&&!strcmp(x[2],"A")&&!strcmp(x[3],"train"));uint32_t session=(uint32_t)number(x[4],39,path),row=(uint32_t)number(x[5],319,path);CHECK(session==rows/320U&&row==rows%320U);char expected_id[32];CHECK(snprintf(expected_id,sizeof(expected_id),"A-train-S%02"PRIu32"-R%03"PRIu32,session,row)>0&&!strcmp(x[1],expected_id));uint64_t time=number(x[6],UINT64_MAX,path);bool valid=number(x[7],1,path)!=0;L1PerceptionInputQuality q=parse_quality(x[8],path);L1Observation o={(uint16_t)number(x[9],10001,path),(uint16_t)number(x[10],10001,path),(uint16_t)number(x[11],10001,path),valid};L1OperationalMemoryStatus status=l1_operational_memory_step(&m,o,q,time);if(status==L1_OM_RESET){last_session=session;last_row=row;cause=q==L1_PQ_RESET?"RESET":q==L1_PQ_REGRESSED?"REGRESSED":"TIME_REGRESSION";}rows++;}
    CHECK(!ferror(f)&&fclose(f)==0);CHECK(rows==7680);CHECK(last_session==22&&last_row==271&&!strcmp(cause,"TIME_REGRESSION"));
    printf("R23C_GATE_A_HISTORICAL rows=%"PRIu32" last_reset=S%02"PRIu32"-R%03"PRIu32" cause=%s post_reset_accepted=%"PRIu32" post_reset_buckets=%u final_status=%d supported=%s\n",rows,last_session,last_row,cause,m.accepted_lifetime,m.windows_seen,(int)m.status,m.status==L1_OM_SUPPORTED?"TRUE":"FALSE");
    CHECK(m.status!=L1_OM_SUPPORTED&&m.windows_seen==3);
}

static void protected_isolation(void){L1Core a,b;L1Shadow sa,sb;L1OperationalMemory memory;l1_init(&a);l1_init(&b);l1_shadow_init(&sa,l1_default_forecast_thresholds());l1_shadow_init(&sb,l1_default_forecast_thresholds());l1_operational_memory_init(&memory);const L1Observation rows[]={{0,0,0,true},{5000,40,100,true},{5000,500,3000,true},{0,0,0,false},{1000,20,700,true}};for(size_t i=0;i<sizeof(rows)/sizeof(rows[0]);i++){L1Action x=l1_step_at(&a,rows[i],i*1000U);(void)l1_operational_memory_step(&memory,rows[i],rows[i].valid?L1_PQ_OK:L1_PQ_INVALID,i*1000U);L1Action y=l1_step_at(&b,rows[i],i*1000U);CHECK(x==y&&memcmp(&a,&b,sizeof(a))==0);L1PressureCounters c={i*10U,i*20U,i*30U,true};(void)l1_shadow_step_at(&sa,rows[i],c,i*1000U);(void)l1_shadow_step_at(&sb,rows[i],c,i*1000U);CHECK(memcmp(&sa,&sb,sizeof(sa))==0);}CHECK(sizeof(L1Core)==104&&sizeof(L1Shadow)==208&&sizeof(L1OperationalMemory)==112);}
int main(int argc,char **argv){if(argc!=2){fprintf(stderr,"usage: %s HISTORICAL_MACHINE_A_TRAIN.csv\n",argv[0]);return EXIT_FAILURE;}known_support_cases();invalid_and_restore_cases();historical_negative(argv[1]);protected_isolation();puts("R23C_GATE_A=PASS cold=N1=N2=N3=N4=PASS support=P1=P2=P3=PASS historical_N5=PASS isolation=PASS");return EXIT_SUCCESS;}
