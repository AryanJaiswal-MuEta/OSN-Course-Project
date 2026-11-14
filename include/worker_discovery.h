#ifndef OSN_WORKER_DISCOVERY_H
#define OSN_WORKER_DISCOVERY_H

#include "worker_config.h"

struct worker_endpoint {
    char ip[16];
    int port;
    int used_fallback;
};

// Startup-only call; worker poll loop ke andar wait mat karna.
int worker_find_scheduler(const struct worker_config *cfg,struct worker_endpoint *out);

#endif
