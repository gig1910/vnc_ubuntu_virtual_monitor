#ifndef VNC_MONITOR_LOG_H
#define VNC_MONITOR_LOG_H

#include <stdarg.h>

typedef enum {
    VNC_LOG_ERROR = 0,
    VNC_LOG_INFO  = 1,
    VNC_LOG_DEBUG = 2,
    VNC_LOG_TRACE = 3
} VncLogLevel;

void vnc_log_set_level(int level);
int vnc_log_get_level(void);
int vnc_log_enabled(int level);
const char *vnc_log_level_name(int level);
int vnc_log_parse_level(const char *value, int *level_out);
void vnc_log_vmessage(int level, const char *format, va_list args);
void vnc_log_message(int level, const char *format, ...);

#define LOG_ERROR(...) vnc_log_message(VNC_LOG_ERROR, __VA_ARGS__)
#define LOG_INFO(...)  vnc_log_message(VNC_LOG_INFO,  __VA_ARGS__)
#define LOG_DEBUG(...) vnc_log_message(VNC_LOG_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) vnc_log_message(VNC_LOG_TRACE, __VA_ARGS__)

#endif
