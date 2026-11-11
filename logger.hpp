#pragma once
#include <stdio.h>
#include <time.h>

#define  LOG(level, format, ...) do{ \
    time_t t = time(NULL); \
    struct tm *lt = localtime(&t); \
    char buffer[32] = { 0 }; \
    strftime(buffer, sizeof(buffer)-1, "%H:%M:%S", lt); \
    fprintf(stdout, "[%s " level " %s:%d]" format "\n", buffer, __FILE__, __LINE__, ##__VA_ARGS__);\
}while(0)

#define ILOG(format, ...) LOG("INFO",  format, ##__VA_ARGS__)
#define DLOG(format, ...) LOG("DEBUG", format, ##__VA_ARGS__)
#define ELOG(format, ...) LOG("ERROR", format, ##__VA_ARGS__)
