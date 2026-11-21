#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "discovery.h"
#include "worker_discovery.h"

int main(int argc,char **argv)
{
    struct worker_config cfg={0};
    struct worker_endpoint ep={0};
    int rc;
    if(argc!=3) return 2;
    cfg.discovery_port=atoi(argv[1]);
    if(!strcmp(argv[2],"fallback")){
        cfg.has_fallback=1;
        strcpy(cfg.scheduler_ip,"127.0.0.1");
        cfg.scheduler_port=9876;
    }
    rc=worker_find_scheduler(&cfg,&ep);
    printf("%d %d %d %s\n",rc,ep.port,ep.used_fallback,ep.ip);
    return 0;
}
