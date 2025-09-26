#ifndef SERIAL_H
#define SERIAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Open and configure a serial port (8N1, no flow control, raw I/O).
    // Returns file descriptor >= 0 on success, or -1 on error.
    int serial_open_configure(const char *dev_path, int baud);

    // Close (safe)
    void serial_close(int fd);

    // Write exactly 1 byte. Returns 1 on success, -1 on error.
    int serial_write_byte(int fd, uint8_t b);

    // Write a buffer (handles partial writes). Returns bytes written or -1 on error.
    ssize_t serial_write_all(int fd, const uint8_t *buf, size_t len);

    // Optional: read with timeout (ms). Returns >0 bytes read, 0 on timeout, -1 on error.
    ssize_t serial_read_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_H
