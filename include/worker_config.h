#ifndef OSN_WORKER_CONFIG_H
#define OSN_WORKER_CONFIG_H

#include "model.h"

#define WORKER_CLAMP_CORES 1
#define WORKER_CLAMP_RAM 2

struct worker_config {
    char scheduler_ip[16];
    int scheduler_port;
    int has_fallback;
    int discovery_port;
    struct resource capacity;
};

int worker_config_parse(int argc,char *const argv[],struct worker_config *cfg,
                        char *err,size_t cap);
int worker_capacity_detect(struct resource *total);
int worker_capacity_limit(struct worker_config *cfg,struct resource total,
                          char *warn,size_t cap);

#endif
