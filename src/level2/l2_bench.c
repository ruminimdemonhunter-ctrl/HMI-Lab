#define _POSIX_C_SOURCE 200809L
#include "l2_parser.h"
#include "l2_state.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

enum { MAX_BENCH_EVENTS=100000,MAX_REPETITIONS=9 };
static uint64_t individual[MAX_BENCH_EVENTS];
static int compare_u64(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return x<y?-1:x>y;}
static uint64_t ns(struct timespec t){return(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;}
static uint64_t now(clockid_t id){struct timespec t;if(clock_gettime(id,&t)!=0){perror("clock_gettime");exit(EXIT_FAILURE);}return ns(t);}
static size_t positive(const char *s,size_t max){if(!s||!s[0]||(s[0]=='0'&&s[1])){fprintf(stderr,"invalid benchmark argument\n");exit(2);}size_t v=0;for(;*s;s++){if(*s<'0'||*s>'9'){fprintf(stderr,"invalid benchmark argument\n");exit(2);}size_t digit=(size_t)(*s-'0');if(digit>max||v>(max-digit)/10U){fprintf(stderr,"invalid benchmark argument\n");exit(2);}v=v*10U+digit;}if(v<1U){fprintf(stderr,"invalid benchmark argument\n");exit(2);}return v;}
int main(int argc,char **argv){if(argc!=3){fprintf(stderr,"usage: %s REPETITIONS EVENTS\n",argv[0]);return 2;}size_t reps=positive(argv[1],MAX_REPETITIONS),events=positive(argv[2],MAX_BENCH_EVENTS);double aggregate[MAX_REPETITIONS],p95max=0.0;uint64_t sink=0;for(size_t r=0;r<reps;r++){L2State state;l2_init(&state);uint64_t cpu0=now(CLOCK_PROCESS_CPUTIME_ID);for(size_t i=0;i<events;i++){char line[L2_MAX_LINE+1U];int n=snprintf(line,sizeof(line),"HMI_L2_EVENT_V1,%zu,%zu,OK,CONTEXT,0,NONE,INTERACTIVE,QUIET,SIMULATED\n",i+1U,i+1U);if(n<=0||(size_t)n>=sizeof(line)){fputs("format failure\n",stderr);return 1;}uint64_t begin=now(CLOCK_PROCESS_CPUTIME_ID);L2Event e;if(!l2_parse_event_line(line,(size_t)n,&e)||l2_step(&state,&e,NULL)!=L2_STEP_ACCEPTED){fputs("benchmark replay failure\n",stderr);return 1;}individual[i]=now(CLOCK_PROCESS_CPUTIME_ID)-begin;sink+=state.context;}uint64_t elapsed=now(CLOCK_PROCESS_CPUTIME_ID)-cpu0;aggregate[r]=(double)elapsed/(double)events;qsort(individual,events,sizeof(individual[0]),compare_u64);double p95=(double)individual[(events*95U-1U)/100U];if(p95>p95max)p95max=p95;printf("L2_BENCH_RUN repetition=%zu events=%zu cpu_ns_per_event=%.3f individual_p95_ns=%.0f\n",r+1U,events,aggregate[r],p95);}for(size_t i=0;i<reps;i++)for(size_t j=i+1;j<reps;j++)if(aggregate[j]<aggregate[i]){double x=aggregate[i];aggregate[i]=aggregate[j];aggregate[j]=x;}struct rusage usage;if(getrusage(RUSAGE_SELF,&usage)!=0){perror("getrusage");return 1;}double median=aggregate[reps/2U];printf("L2_BENCH_SUMMARY repetitions=%zu events=%zu median_cpu_ns_per_event=%.3f max_individual_p95_ns=%.0f rss_kib=%ld state_bytes=%zu sink=%"PRIu64"\n",reps,events,median,p95max,usage.ru_maxrss,sizeof(L2State),sink);if(median>50000.0||p95max>250000.0||usage.ru_maxrss>8192||sizeof(L2State)>8192){puts("L2_BENCH=FAIL");return 1;}puts("L2_BENCH=PASS");return 0;}
