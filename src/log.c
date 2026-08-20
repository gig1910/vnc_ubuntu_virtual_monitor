#define _GNU_SOURCE

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int current_log_level = VNC_LOG_INFO;

void
vnc_log_set_level(int level)
{
    if (level < VNC_LOG_ERROR)
        level = VNC_LOG_ERROR;
    if (level > VNC_LOG_TRACE)
        level = VNC_LOG_TRACE;

    current_log_level = level;
}

int
vnc_log_get_level(void)
{
    return current_log_level;
}

int
vnc_log_enabled(int level)
{
    return level <= current_log_level;
}

const char *
vnc_log_level_name(int level)
{
    switch (level) {
        case VNC_LOG_ERROR:
            return "error";
        case VNC_LOG_INFO:
            return "info";
        case VNC_LOG_DEBUG:
            return "debug";
        case VNC_LOG_TRACE:
            return "trace";
        default:
            return "unknown";
    }
}

int
vnc_log_parse_level(const char *value, int *level_out)
{
    if (!value || !*value || !level_out)
        return -1;

    if (strcmp(value, "0") == 0 || strcasecmp(value, "error") == 0 ||
        strcasecmp(value, "quiet") == 0) {
        *level_out = VNC_LOG_ERROR;
        return 0;
    }

    if (strcmp(value, "1") == 0 || strcasecmp(value, "info") == 0) {
        *level_out = VNC_LOG_INFO;
        return 0;
    }

    if (strcmp(value, "2") == 0 || strcasecmp(value, "debug") == 0) {
        *level_out = VNC_LOG_DEBUG;
        return 0;
    }

    if (strcmp(value, "3") == 0 || strcasecmp(value, "trace") == 0) {
        *level_out = VNC_LOG_TRACE;
        return 0;
    }

    return -1;
}

void
vnc_log_vmessage(int level, const char *format, va_list args)
{
    if (!format || !vnc_log_enabled(level))
        return;

    flockfile(stderr);
    fprintf(stderr, "[%s] ", vnc_log_level_name(level));
    vfprintf(stderr, format, args);

    size_t len = strlen(format);
    if (len == 0 || format[len - 1] != '\n')
        fputc('\n', stderr);

    fflush(stderr);
    funlockfile(stderr);
}

void
vnc_log_message(int level, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vnc_log_vmessage(level, format, args);
    va_end(args);
}
