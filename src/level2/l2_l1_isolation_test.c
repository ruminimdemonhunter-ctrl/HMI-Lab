#include "l2_state.h"
#include "../level1/l1.h"
#include "../level1/l1_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do{if(!(c)){fprintf(stderr,"L2_L1_ISOLATION_FAIL line=%d\n",__LINE__);exit(EXIT_FAILURE);}}while(0)
int main(void){L1Core a,b;L1Shadow sa,sb;L2State l2;l1_init(&a);l1_init(&b);l1_shadow_init(&sa,l1_default_forecast_thresholds());l1_shadow_init(&sb,l1_default_forecast_thresholds());l2_init(&l2);const L1Observation obs[]={{0,0,0,true},{5000,50,100,true},{5000,500,3000,true},{0,0,0,false},{1000,20,700,true}};for(size_t i=0;i<sizeof(obs)/sizeof(obs[0]);i++){L1Action x=l1_step_at(&a,obs[i],i*1000U);L2Event e={i+1U,i+1U,L2_QUALITY_OK,L2_EVENT_CONTEXT,0,L2_ROLE_NONE,L2_ACTIVITY_INTERACTIVE,L2_CONDITION_QUIET,true};CHECK(l2_step(&l2,&e,NULL)==L2_STEP_ACCEPTED);L1Action y=l1_step_at(&b,obs[i],i*1000U);CHECK(x==y&&memcmp(&a,&b,sizeof(a))==0);L1PressureCounters c={i*10U,i*20U,i*30U,true};(void)l1_shadow_step_at(&sa,obs[i],c,i*1000U);(void)l1_shadow_step_at(&sb,obs[i],c,i*1000U);CHECK(memcmp(&sa,&sb,sizeof(sa))==0);}CHECK(sizeof(L1Core)==104&&sizeof(L1Shadow)==208&&sizeof(L2State)<=8192);printf("PASS: L2 observer cannot alter L1 action/core/shadow bytes; l2_state_bytes=%zu\n",sizeof(L2State));return 0;}
