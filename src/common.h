// Shared domain types.
#ifndef GENDA_COMMON_H
#define GENDA_COMMON_H

// Normalized input for both Android notifications and email.
typedef struct {
  char source[32], app[128], title[512], text[4096], from[256], time[64], ext_id[128];
} Input;

// Classification result. Mock = keyword heuristic (see classify.c TODO).
typedef struct {
  char title[512], starts_at[64], deadline[64], location[256], kind[16];
  double confidence;
} Classified;

#endif
