#define _POSIX_C_SOURCE 200809L
#include "l2_parser.h"
#include "l2_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *name){fprintf(stderr,"usage: %s SYNTHETIC_FIXTURE.csv\n",name);}
int main(int argc,char **argv){if(argc!=2){usage(argv[0]);return 2;}FILE *f=fopen(argv[1],"rb");if(!f){perror("fixture open");return 2;}char line[L2_MAX_LINE+2U];size_t bytes=0,rows=0,queries=0;if(!fgets(line,sizeof(line),f)||!l2_parse_header(line,strlen(line))){fprintf(stderr,"L2_REJECT reason=HEADER\n");fclose(f);return 1;}bytes=strlen(line);L2State state;l2_init(&state);while(fgets(line,sizeof(line),f)){size_t n=strlen(line);bytes+=n;if(n==0||n>L2_MAX_LINE||line[n-1U]!='\n'||bytes>L2_MAX_FILE_BYTES||++rows>L2_MAX_ROWS){fprintf(stderr,"L2_REJECT reason=BOUNDS row=%zu\n",rows);fclose(f);return 1;}L2Event event;if(!l2_parse_event_line(line,n,&event)){fprintf(stderr,"L2_REJECT reason=PARSE row=%zu\n",rows);fclose(f);return 1;}L2QueryResult result;L2StepStatus status=l2_step(&state,&event,&result);if(status==L2_STEP_REJECTED){fprintf(stderr,"L2_REJECT reason=STATE row=%zu\n",rows);fclose(f);return 1;}if(status==L2_STEP_QUERY_RESULT){queries++;printf("L2_QUERY workspace=%u status=%s context=%s reason=%s roles=%s|%s|%s sessions=%u transitions=%u triple_count=%u approval=%s\n",result.workspace,l2_result_name(result.status),l2_context_name(result.context),l2_reason_name(result.reason),l2_role_name(result.roles[0]),l2_role_name(result.roles[1]),l2_role_name(result.roles[2]),result.completed_sessions,result.accepted_transitions,result.triple_count,result.requires_user_approval?"REQUIRED":"NO");}}
if(ferror(f)||fclose(f)!=0){fprintf(stderr,"L2_REJECT reason=READ\n");return 1;}printf("L2_SUMMARY rows=%zu queries=%zu state_bytes=%zu authority=TEXT_ONLY os_writes=NONE provenance=SIMULATED\n",rows,queries,sizeof(state));return 0;}
