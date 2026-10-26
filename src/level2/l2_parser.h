#ifndef HMI_L2_PARSER_H
#define HMI_L2_PARSER_H

#include "l2_state.h"

#include <stdbool.h>
#include <stddef.h>

enum { L2_MAX_LINE=256, L2_MAX_ROWS=100000, L2_MAX_FILE_BYTES=16*1024*1024 };

bool l2_parse_header(const char *line,size_t length);
bool l2_parse_event_line(const char *line,size_t length,L2Event *event);

#endif
