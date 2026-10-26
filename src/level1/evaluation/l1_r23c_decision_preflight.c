#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"R23C_GATE_B_FAIL line=%d check=%s\n",__LINE__,#c); exit(EXIT_FAILURE); } } while (0)
typedef enum { DECISION_UNKNOWN=0,DECISION_EXPECTED=1,DECISION_SUSPECTED=2 } Decision;
typedef enum { MODEL_NOVELTY=0,MODEL_FIXED=1 } Model;
static bool valid_fixed_k(uint16_t k){return k==1||k==3||k==5||k==8||k==12;}
static Decision decide(Model model,uint16_t eligible,uint16_t hits,uint16_t fixed_k){if(eligible<10)return DECISION_UNKNOWN;uint16_t k;if(model==MODEL_NOVELTY)k=5;else{if(!valid_fixed_k(fixed_k))return DECISION_UNKNOWN;k=fixed_k;}return hits>=k?DECISION_SUSPECTED:DECISION_EXPECTED;}
static Decision old_bug(uint16_t eligible,uint16_t hits,uint16_t high_k){if(eligible<10)return DECISION_UNKNOWN;return hits>=high_k?DECISION_SUSPECTED:DECISION_EXPECTED;}
int main(void){const uint16_t expected_hits[]={0,1,4};for(size_t i=0;i<3;i++)CHECK(decide(MODEL_NOVELTY,10,expected_hits[i],1)==DECISION_EXPECTED);CHECK(decide(MODEL_NOVELTY,10,5,12)==DECISION_SUSPECTED);CHECK(decide(MODEL_NOVELTY,10,6,1)==DECISION_SUSPECTED);for(uint16_t hits=0;hits<=9;hits++)CHECK(decide(MODEL_NOVELTY,9,hits,5)==DECISION_UNKNOWN);const uint16_t ks[]={1,3,5,8,12};for(size_t i=0;i<5;i++){CHECK(decide(MODEL_NOVELTY,10,4,ks[i])==DECISION_EXPECTED);CHECK(decide(MODEL_NOVELTY,10,5,ks[i])==DECISION_SUSPECTED);}CHECK(!valid_fixed_k(0));CHECK(decide(MODEL_FIXED,10,0,0)==DECISION_UNKNOWN);CHECK(old_bug(10,0,0)==DECISION_SUSPECTED);CHECK(old_bug(10,0,0)!=decide(MODEL_NOVELTY,10,0,5));puts("R23C_GATE_B=PASS novelty_k=5 comparator_k_independent=PASS k0_rejected=PASS old_bug_mutation_caught=PASS");return EXIT_SUCCESS;}
