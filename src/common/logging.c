#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "logging.h"

void log_line(const char *component,const char *fmt,...)
{
    time_t now=time(NULL);
    struct tm tm;
    char stamp[16];
    va_list args;
    if(!component||!fmt) return;
    localtime_r(&now,&tm); strftime(stamp,sizeof(stamp),"%H:%M:%S",&tm);
    // Ek hi helper se component logs ka format same rahega.
    printf("[%s] [%s] ",stamp,component);
    va_start(args,fmt); vprintf(fmt,args); va_end(args);
    putchar('\n'); fflush(stdout);
}
