#ifndef OSN_WORKER_INTERNAL_H
#define OSN_WORKER_INTERNAL_H

#include "runner.h"

int run_prepare(struct run_job *j,const char *path);
int run_cleanup(struct run_job *j);
void run_sample(struct run_job *j);

#endif
