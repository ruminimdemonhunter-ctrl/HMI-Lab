#define _POSIX_C_SOURCE 200809L
#include "l1_novelty.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

enum { MIN_CALLS=10000, MAX_CALLS=100000000, MAX_REPETITIONS=100,
       MAX_SAMPLES=50000 };

static uint64_t bounded(const char *text,uint64_t low,uint64_t high,const char *name){
    char *end=NULL; errno=0; unsigned long long value=strtoull(text,&end,10);
    if(errno!=0||text[0]=='\0'||text[0]=='-'||end==text||*end!='\0'||value<low||value>high){
        fprintf(stderr,"invalid %s: expected %" PRIu64 "..%" PRIu64 "\n",name,low,high); exit(EXIT_FAILURE);
    } return (uint64_t)value;
}
static uint64_t cpu_ns(void){struct timespec t;if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)!=0){perror("clock_gettime");exit(EXIT_FAILURE);}return(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;}
static int cmp_double(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}
static int cmp_u64(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return(x>y)-(x<y);}
static L1Observation observation(uint64_t i){return(L1Observation){(uint16_t)(i*1103U%10001U),(uint16_t)(i*337U%10001U),(uint16_t)(i*719U%10001U),true};}
static double median(double *v,size_t n){qsort(v,n,sizeof(*v),cmp_double);return(n&1U)?v[n/2U]:(v[n/2U-1U]+v[n/2U])/2.0;}
static uint64_t p95(uint64_t *v,size_t n){qsort(v,n,sizeof(*v),cmp_u64);return v[(n*95U+99U)/100U-1U];}
static L1OperationalMemorySummary make_reference(void){
    L1OperationalMemory memory;l1_operational_memory_init(&memory);
    for(size_t i=0;i<600;++i)(void)l1_operational_memory_step(&memory,observation(i),L1_PQ_OK,(uint64_t)(i/100U)*L1_OM_WINDOW_MS+(uint64_t)(i%100U)*L1_OM_SAMPLE_INTERVAL_MS);
    return l1_operational_memory_summary(&memory);
}
int main(int argc,char **argv){
    if(argc!=3){fprintf(stderr,"usage: %s REPETITIONS CALLS_PER_REPETITION\n",argv[0]);return EXIT_FAILURE;}
    size_t repetitions=(size_t)bounded(argv[1],1,MAX_REPETITIONS,"repetitions");uint64_t calls=bounded(argv[2],MIN_CALLS,MAX_CALLS,"calls");
    double runs[MAX_REPETITIONS];uint64_t samples[MAX_SAMPLES];size_t sample_count=0;volatile uint64_t sink=0;
    L1OperationalMemorySummary reference=make_reference();
    for(size_t r=0;r<repetitions;++r){L1OperationalMemory live;l1_operational_memory_init(&live);uint64_t start=cpu_ns();
        for(uint64_t i=0;i<calls;++i){L1Observation o=observation(i);L1PerceptionResult p=l1_perception_classify(o,L1_PQ_OK);L1OperationalMemoryStatus s=l1_operational_memory_step(&live,o,L1_PQ_OK,i*L1_OM_SAMPLE_INTERVAL_MS);L1NoveltyResult n=l1_novelty_score(&reference,o,L1_PQ_OK,768);sink+=(uint64_t)p.state+p.pressure_mask+s+n.novelty_score+n.disposition;}
        runs[r]=(double)(cpu_ns()-start)/(double)calls;printf("NOVELTY_RUN repetition=%zu calls=%" PRIu64 " combined_cpu_ns_per_call=%.3f\n",r+1U,calls,runs[r]);
        size_t wanted=MAX_SAMPLES/repetitions;if((uint64_t)wanted>calls)wanted=(size_t)calls;l1_operational_memory_init(&live);
        for(size_t i=0;i<wanted;++i){L1Observation o=observation(i);uint64_t one=cpu_ns();L1PerceptionResult p=l1_perception_classify(o,L1_PQ_OK);L1OperationalMemoryStatus s=l1_operational_memory_step(&live,o,L1_PQ_OK,(uint64_t)i*L1_OM_SAMPLE_INTERVAL_MS);L1NoveltyResult n=l1_novelty_score(&reference,o,L1_PQ_OK,768);samples[sample_count++]=cpu_ns()-one;sink+=(uint64_t)p.state+s+n.novelty_score;}
    }
    double med=median(runs,repetitions);uint64_t q=p95(samples,sample_count);struct rusage usage;if(getrusage(RUSAGE_SELF,&usage)!=0){perror("getrusage");return EXIT_FAILURE;}
    printf("NOVELTY_SUMMARY repetitions=%zu calls_per_repetition=%" PRIu64 " combined_cpu_ns=%.3f/%.3f/%.3f individual_samples=%zu combined_individual_p95_ns=%" PRIu64 " rss_kib=%ld memory_bytes=%zu summary_bytes=%zu novelty_result_bytes=%zu sink=%" PRIu64 "\n",repetitions,calls,runs[0],med,runs[repetitions-1U],sample_count,q,usage.ru_maxrss,sizeof(L1OperationalMemory),sizeof(L1OperationalMemorySummary),sizeof(L1NoveltyResult),sink);
    if(med>1000.0||q>5000U||usage.ru_maxrss>8192||sizeof(L1OperationalMemory)>512U){fputs("FAIL: frozen R2.3 resource budget exceeded\n",stderr);return EXIT_FAILURE;}
    puts("PASS: bounded perception-memory-novelty benchmark");return EXIT_SUCCESS;
}
