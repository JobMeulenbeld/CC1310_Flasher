#define _XOPEN_SOURCE 600
#include "serial.h"
#include "sbl.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


// --- small I/O helpers ---
static int read_exact_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t got = 0;
    while (got < len)
    {
        ssize_t n = serial_read_timeout(fd, buf + got, len - got, timeout_ms);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0; // timeout
        got += (size_t)n;
    }
    return 1;
}

// --- SBL core ---

// Wait for ACK or NACK, tolerate leading 0x00 or noise.
// Returns 0 on ACK, -1 on NACK/timeout/error.
static int sbl_wait_ack(int fd, int timeout_ms)
{
    const int step_ms = 20; // poll slice
    int waited = 0;

    while (waited < timeout_ms)
    {
        uint8_t b;
        ssize_t n = serial_read_timeout(fd, &b, 1, step_ms);
        if (n < 0)
            return -1; // hard error
        if (n == 1)
        {
            if (b == SBL_ACK)
                return 0; // 0xCC = ACK
            if (b == SBL_NACK)
            {
                errno = EPROTO;
                return -1;
            }
            // Ignore everything else (0x00 noise etc.)
        }
        waited += step_ms;
    }
    errno = ETIMEDOUT;
    return -1;
}

int sbl_autobaud(int fd, int timeout_ms)
{
    uint8_t ub = 0x55;
    // burst a few 0x55 to help the BL lock
    for (int i = 0; i < 2; ++i)
        if (serial_write_byte(fd, ub) != 1)
            return -1;

    int elapsed = 0;
    const int step_ms = 20;
    uint8_t b = 0;

    while (elapsed < timeout_ms)
    {
        ssize_t n = serial_read_timeout(fd, &b, 1, step_ms);
        if (n < 0)
            return -1; // real error
        if (n == 1)
        {
            if (b == 0xCC)
                return 0; // ACK!

            // Many ROMs/ACM stacks spit 0x00 before ACK; ignore it (and any other noise)
        }
        elapsed += step_ms;
    }
    errno = ETIMEDOUT;
    return -1;
}

int sbl_autobaud_scan(const char *dev_path,
                      const int *bauds, size_t n_bauds,
                      int timeout_ms, int *baud_ok)
{
    if (!dev_path || !bauds || n_bauds == 0)
    {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < n_bauds; ++i)
    {
        int b = bauds[i];
        int fd = serial_open_configure(dev_path, b);
        if (fd < 0)
            continue;

        // small settle
        usleep(1000 * 10);

        int ok = sbl_autobaud(fd, timeout_ms);
        serial_close(fd);

        if (ok == 0)
        {
            if (baud_ok)
                *baud_ok = b;
            return 0;
        }
    }
    return -1;
}

static uint8_t checksum_sum(const uint8_t *data, size_t len)
{
    unsigned sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += data[i];
    return (uint8_t)(sum & 0xFF);
}

// Send one SBL packet and wait for ACK; if bootloader is sender in response,
// this function can read the response packet (size+checksum+payload) into out.
int sbl_send_cmd(int fd, const uint8_t *data, size_t len,
                 uint8_t *out, size_t out_max, int timeout_ms)
{
    if (!data || len == 0 || len > 253)
    {
        errno = EINVAL;
        return -1;
    }

    uint8_t size = (uint8_t)(len + 2); // includes SIZE itself and checksum size
    uint8_t csum = checksum_sum(data, len);

    // Build frame: [SIZE][CHECKSUM][DATA...]
    uint8_t frame[2 + 253];
    frame[0] = size;
    frame[1] = csum;
    memcpy(&frame[2], data, len);

    // *** single write — avoids inter-byte gaps on USB CDC/FTDI ***
    if (serial_write_all(fd, frame, 2 + len) < 0)
        return -1;

    // Read ACK/NACK
    if (sbl_wait_ack(fd, timeout_ms) != 0)
        return -1;

    // Some commands return nothing else. If caller wants a response, try to read one packet.
    if (out && out_max)
    {
        // Peek for a non-zero size byte within timeout; if timeout, just return success with no data.
        uint8_t sz = 0;
        int r = read_exact_timeout(fd, &sz, 1, 50);
        if (r > 0 && sz != 0)
        {
            uint8_t rx_csum = 0;
            if (read_exact_timeout(fd, &rx_csum, 1, timeout_ms) <= 0)
                return -1;

            size_t payload_len = (size_t)sz - 2;
            if (payload_len > out_max)
            {
                errno = EMSGSIZE;
                return -1;
            }
            if (read_exact_timeout(fd, out, payload_len, timeout_ms) <= 0)
                return -1;

            // ACK the device’s response frame
            uint8_t ack[2] = {0x00, SBL_ACK};
            if (serial_write_all(fd, ack, 2) < 0)
                return -1;

            if (checksum_sum(out, payload_len) != rx_csum)
            {
                errno = EPROTO;
                return -1;
            }
            return (int)payload_len;
        }
    }

    return 0;
}

// --- Convenience commands ---
int sbl_ping(int fd, int timeout_ms)
{
    uint8_t cmd = CMD_PING;
    return sbl_send_cmd(fd, &cmd, 1, NULL, 0, timeout_ms);
}

int sbl_get_status(int fd, int timeout_ms, uint8_t *status_out)
{
    uint8_t cmd = CMD_GET_STATUS;
    uint8_t resp[1];
    int n = sbl_send_cmd(fd, &cmd, 1, resp, sizeof(resp), timeout_ms);
    if (n < 0)
        return -1;
    if (n == 1 && status_out)
        *status_out = resp[0];
    return 0;
}

int sbl_get_chip_id(int fd, int timeout_ms, uint32_t *chip_id_out)
{
    uint8_t cmd = CMD_GET_CHIP_ID;
    uint8_t resp[4];
    int n = sbl_send_cmd(fd, &cmd, 1, resp, sizeof(resp), timeout_ms);
    if (n < 0)
        return -1;
    if (n == 4 && chip_id_out)
    {
        // Little-endian 32-bit
        *chip_id_out = (uint32_t)resp[0] |
                       ((uint32_t)resp[1] << 8) |
                       ((uint32_t)resp[2] << 16) |
                       ((uint32_t)resp[3] << 24);
    }
    return 0;
}

int sbl_reset(int fd, int timeout_ms)
{
    uint8_t cmd = CMD_RESET;
    return sbl_send_cmd(fd, &cmd, 1, NULL, 0, timeout_ms);
}

int sbl_download(int fd, uint32_t addr, uint32_t total_len, int timeout_ms)
{
    // Command + 4byte address + 4byte length
    uint8_t msg[1 + 4 + 4];
    msg[0] = CMD_DOWNLOAD;
    msg[1] = (uint8_t)(addr >> 24);
    msg[2] = (uint8_t)(addr >> 16);
    msg[3] = (uint8_t)(addr >> 8);
    msg[4] = (uint8_t)(addr);
    msg[5] = (uint8_t)(total_len >> 24);
    msg[6] = (uint8_t)(total_len >> 16);
    msg[7] = (uint8_t)(total_len >> 8);
    msg[8] = (uint8_t)(total_len);
    return sbl_send_cmd(fd, msg, sizeof(msg), NULL, 0, timeout_ms);
}

int sbl_sector_erase(int fd, uint32_t addr, int timeout_ms)
{
    uint8_t msg[1 + 4];
    msg[0] = CMD_SECTOR_ERASE;
    msg[1] = (uint8_t)(addr >> 24);
    msg[2] = (uint8_t)(addr >> 16);
    msg[3] = (uint8_t)(addr >> 8);
    msg[4] = (uint8_t)(addr);
    return sbl_send_cmd(fd, msg, sizeof(msg), NULL, 0, timeout_ms);
}

int sbl_send_data(int fd, const uint8_t *chunk, size_t n, int timeout_ms)
{
    if (!chunk || n == 0 || n > 252)
    {
        errno = EINVAL;
        return -1;
    }

    uint8_t msg[1 + 252];
    msg[0] = CMD_SEND_DATA;
    memcpy(&msg[1], chunk, n);

    // data length = 1 (cmd) + n
    return sbl_send_cmd(fd, msg, (size_t)(1 + n), NULL, 0, timeout_ms);
}

int sbl_crc32(int fd, uint32_t addr, uint32_t len, uint32_t repeat, int timeout_ms, uint32_t *crc_out)
{
    // Command + 4-byte addr + 4-byte len + 4-byte repeat
    uint8_t msg[1 + 4 + 4 + 4];
    msg[0] = CMD_CRC32;
    msg[1] = (uint8_t)(addr >> 24);
    msg[2] = (uint8_t)(addr >> 16);
    msg[3] = (uint8_t)(addr >> 8);
    msg[4] = (uint8_t)(addr);
    msg[5] = (uint8_t)(len >> 24);
    msg[6] = (uint8_t)(len >> 16);
    msg[7] = (uint8_t)(len >> 8);
    msg[8] = (uint8_t)(len);
    msg[9] = (uint8_t)(repeat >> 24);
    msg[10] = (uint8_t)(repeat >> 16);
    msg[11] = (uint8_t)(repeat >> 8);
    msg[12] = (uint8_t)(repeat);

    uint8_t resp[4];
    int n = sbl_send_cmd(fd, msg, sizeof(msg), resp, sizeof(resp), timeout_ms);
    if (n < 0)
        return -1;
    if (n != 4)
    {
        errno = EPROTO;
        return -1;
    }

    if (crc_out)
    {
        *crc_out = (uint32_t)resp[0] << 24 |
                   ((uint32_t)resp[1] << 16) |
                   ((uint32_t)resp[2] << 8) |
                   ((uint32_t)resp[3]);
    }
    return 0;
}

int sbl_program_binary(int fd,
                       uint32_t flash_size, uint32_t page_size,
                       const uint8_t *image, size_t image_len,
                       uint32_t base_addr)
{
    uint32_t perc = 0;

    if (base_addr % page_size)
    {
        fprintf(stderr, "Error: base_addr 0x%08X not page aligned (page size %u)\n", base_addr, page_size);
        return -1;
    }

    // Erase only what’s needed (rounded up to page size)
    uint32_t erase_len = (image_len + page_size - 1) & ~(page_size - 1);
    uint32_t last_page_start = flash_size - page_size; // CCFG
    if (base_addr + erase_len > last_page_start)
        erase_len = last_page_start - base_addr;

    for (uint32_t a = base_addr; a < base_addr + erase_len; a += page_size)
    {
        if (sbl_sector_erase(fd, a, 5000) != 0)
        {
            fprintf(stderr, "Erase failed at 0x%08X\n", a);
            return -1;
        }
        printf("Erased 0x%08X\n", a);
        uint8_t st = 0;
        if (sbl_get_status(fd, 1000, &st) != 0)
        {
            perror("GET_STATUS after ERASE");
            return -1;
        }
        printf("ERASE status: 0x%02X\n", st); // expect 0x40
    }

    // Calculate the image lenght + needed padding to allign by 4
    size_t remainder = image_len % 4;
    size_t total_len = image_len;
    if (remainder != 0)
    {
        total_len += (4 - remainder);
    }

    if (sbl_download(fd, base_addr, (uint32_t)total_len, 1000) != 0)
    {
        fprintf(stderr, "DOWNLOAD failed\n");
        return -1;
    }

    uint8_t st = 0;
    if (sbl_get_status(fd, 500, &st) != 0)
    {
        fprintf(stderr, "GET_STATUS after DOWNLOAD failed\n");
        return -1;
    }
    if (st != 0x40)
    { // 0x40 = success per your table
        fprintf(stderr, "DOWNLOAD rejected: status 0x%02X\n", st);
        return -1;
    }

    // send exactly total_len bytes in <=252-byte chunks, padding with 0xFF as needed
    for (size_t off = 0; off < total_len;)
    {
        size_t chunk_len = total_len - off;
        if (chunk_len > 252)
            chunk_len = 252;

        uint8_t buf[252];
        size_t copy_len = chunk_len;
        if (off + copy_len > image_len)
            copy_len = image_len - off; // may be 0 near the end

        if (copy_len)
            memcpy(buf, &image[off], copy_len);
        for (size_t i = copy_len; i < chunk_len; ++i)
            buf[i] = 0xFF; // pad last chunk

        if (sbl_send_data(fd, buf, chunk_len, 1000) != 0)
        {
            fprintf(stderr, "SEND_DATA failed at offset %zu\n", off);
            return -1;
        }

        uint8_t st = 0;
        if (sbl_get_status(fd, 500 /*ms*/, &st) != 0)
        {
            fprintf(stderr, "GET_STATUS failed after offset %zu\n", off);
            return -1;
        }
        if (st != 0x40)
        { // 0x40 is the ROM's "SUCCESS" on many parts; print whatever you see.
            fprintf(stderr, "Prog status != SUCCESS (0x%02X) at offset %zu\n", st, off);
            return -1;
        }

        off += chunk_len;

        uint32_t calc_perc = ((double) off / (double)total_len) * 100.00;
        if(calc_perc != perc)
        {
            printf("Progress: %d%%\n", perc);
            perc = calc_perc;
        }
    }

    // Optional reset into app
    sbl_reset(fd, 1000);

    return 0;
}