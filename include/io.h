#ifndef VNC_MONITOR_IO_H
#define VNC_MONITOR_IO_H

#include <stddef.h>
#include <stdint.h>

/* 1=success, 0=EOF, -1=error */
int io_read_exact_status(int fd, void *buf, size_t len);
int io_read_exact(int fd, void *buf, size_t len);
int io_write_exact(int fd, const void *buf, size_t len);

uint16_t io_get_u16_be(const uint8_t *p);
uint32_t io_get_u32_be(const uint8_t *p);
void io_put_u16_be(uint8_t *p, uint16_t v);
void io_put_u32_be(uint8_t *p, uint32_t v);

#endif
