#define _POSIX_C_SOURCE 200809L
#include "serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// Map integer baud to termios speed_t
static speed_t baud_to_speed_t(int baud) {
    switch (baud) {
        case 50: return B50; case 75: return B75; case 110: return B110; case 134: return B134;
        case 150: return B150; case 200: return B200; case 300: return B300; case 600: return B600;
        case 1200: return B1200; case 1800: return B1800; case 2400: return B2400; case 4800: return B4800;
        case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
#ifdef B57600
        case 57600: return B57600;
#endif
#ifdef B115200
        case 115200: return B115200;
#endif
#ifdef B230400
        case 230400: return B230400;
#endif
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default: return 0;
    }
}

int serial_open_configure(const char *dev_path, int baud) {
    if (!dev_path) { errno = EINVAL; return -1; }

    int fd = open(dev_path, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    // Set raw mode (disables canonical mode, echo, translations, etc.)
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
               | INLCR | IGNCR | ICRNL | IXON);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~(CSIZE | PARENB);
    tio.c_cflag |= CS8;

    // 8N1, enable receiver, ignore modem control lines
    #ifdef CRTSCTS
        tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    #else
        tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    #endif
    tio.c_cflag |= (CS8 | CLOCAL | CREAD);

    // No software flow control
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);

    // Blocking read behavior controlled by VMIN/VTIME; we’ll default to non-blocking by itself,
    // and provide a poll-based timeout in serial_read_timeout().
    tio.c_cc[VMIN]  = 0;  // return immediately if no data
    tio.c_cc[VTIME] = 0;  // no interbyte timer

    // Baud rate
    speed_t spd = baud_to_speed_t(baud);
    if (spd == 0) {
        fprintf(stderr, "Unsupported baud: %d\n", baud);
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (cfsetispeed(&tio, spd) < 0 || cfsetospeed(&tio, spd) < 0) {
        perror("cfset*speed");
        close(fd);
        return -1;
    }

    // Apply immediately
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    // Clear pending I/O
    tcflush(fd, TCIOFLUSH);
    return fd;
}

void serial_close(int fd) {
    if (fd >= 0) close(fd);
}

int serial_write_byte(int fd, uint8_t b) {
    ssize_t n = write(fd, &b, 1);
    if (n < 0) return -1;
    // Ensure it's pushed out to the driver
    if (tcdrain(fd) < 0) return -1;
    return (int)n;
}

ssize_t serial_write_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    // Block until transmitted
    if (tcdrain(fd) < 0) return -1;
    return (ssize_t)sent;
}

ssize_t serial_read_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;          // error
    if (pr == 0) return 0;          // timeout
    if (pfd.revents & POLLIN) {
        ssize_t n = read(fd, buf, len);
        if (n < 0) return -1;
        return n;
    }
    return 0;
}
