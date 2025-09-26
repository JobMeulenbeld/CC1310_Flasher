#define _POSIX_C_SOURCE 200809L
#include "serial.h"
#include "sbl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Parse one byte from string: supports "0xA5", "A5", or decimal "165"
static int parse_byte(const char *s, unsigned *out)
{
    if (!s || !out)
        return -1;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0); // base 0: auto-detect 0x.., 0.., decimal
    if (end == s || *end != '\0' || v > 255UL)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s <dev> <baud> txbyte <byte>\n"
            "  %s <dev> <baud> tx <b0> <b1> ... <bn>\n"
            "  %s <dev> <baud> rx <timeout_ms>\n"
            "  %s <dev> <baud> sbl_autobaud\n"
            "  %s <dev> <baud> sbl_autobaud_scan\n"
            "  %s <dev> <baud> sbl_ping\n"
            "  %s <dev> <baud> sbl_status\n"
            "  %s <dev> <baud> sbl_chipid\n"
            "  %s <dev> <baud> sbl_reset\n"
            "  %s <dev> <baud> sbl_erase <addr_hex>\n"
            "  %s <dev> <baud> sbl_full_erase <flash_size_hex> <page_size_hex>\n"
            "  %s <dev> <baud> sbl_send_data <b0> <b1> ... <bn>\n"
            "  %s <dev> <baud> sbl_crc <addr_hex> <len> <repeat>\n"
            "  %s <dev> <baud> sbl_program <bin_location> <addr_hex> <flash_size_hex> <page_size_hex>\n",
            prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

unsigned char* load_bin(const char* path, size_t* out_len) {
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);

    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return NULL; }

    *out_len = n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        usage(argv[0]);
        return 1;
    }

    const char *dev = argv[1];
    int baud = atoi(argv[2]);
    const char *cmd = argv[3];

    int fd = serial_open_configure(dev, baud);
    if (fd < 0)
    {
        fprintf(stderr, "Failed to open %s at %d baud: %s\n", dev, baud, strerror(errno));
        return 2;
    }

    int rc = 0;

    if (strcmp(cmd, "txbyte") == 0)
    {
        if (argc != 5)
        {
            usage(argv[0]);
            rc = 1;
            goto done;
        }
        unsigned v;
        if (parse_byte(argv[4], &v) != 0)
        {
            fprintf(stderr, "Invalid byte: %s\n", argv[4]);
            rc = 1;
            goto done;
        }
        if (serial_write_byte(fd, (uint8_t)v) != 1)
        {
            fprintf(stderr, "Write failed: %s\n", strerror(errno));
            rc = 3;
            goto done;
        }
        printf("Sent 1 byte: 0x%02X\n", (unsigned)v);
    }
    else if (strcmp(cmd, "tx") == 0)
    {
        if (argc < 5)
        {
            usage(argv[0]);
            rc = 1;
            goto done;
        }

        size_t count = (size_t)(argc - 4);
        uint8_t *buf = (uint8_t *)malloc(count);
        if (!buf)
        {
            perror("malloc");
            rc = 4;
            goto done;
        }

        for (size_t i = 0; i < count; ++i)
        {
            unsigned v;
            if (parse_byte(argv[4 + (int)i], &v) != 0)
            {
                fprintf(stderr, "Invalid byte: %s\n", argv[4 + (int)i]);
                free(buf);
                rc = 1;
                goto done;
            }
            buf[i] = (uint8_t)v;
        }

        ssize_t n = serial_write_all(fd, buf, count);
        if (n < 0)
        {
            fprintf(stderr, "Write failed: %s\n", strerror(errno));
            free(buf);
            rc = 3;
            goto done;
        }
        printf("Sent %zd bytes.\n", n);
        free(buf);
    }
    else if (strcmp(cmd, "rx") == 0)
    {
        if (argc != 5)
        {
            usage(argv[0]);
            rc = 1;
            goto done;
        }

        int timeout_ms = atoi(argv[4]);
        if (timeout_ms < 0)
            timeout_ms = 0;

        uint8_t buf[256];
        ssize_t n = serial_read_timeout(fd, buf, sizeof(buf), timeout_ms);
        if (n < 0)
        {
            fprintf(stderr, "Read failed: %s\n", strerror(errno));
            rc = 5;
            goto done;
        }
        if (n == 0)
        {
            printf("Timeout: no data received.\n");
        }
        else
        {
            printf("Received %zd bytes:\n", n);
            for (ssize_t i = 0; i < n; i++)
            {
                printf("0x%02X ", buf[i]);
            }
            printf("\n");
        }
    }
    else if (strcmp(cmd, "sbl_autobaud") == 0)
    {
        if (sbl_autobaud(fd, 500) != 0)
        {
            fprintf(stderr, "Auto-baud failed.\n");
            rc = 1;
            goto done;
        }
        printf("Auto-baud OK (ACK 0xCC).\n");
    }
    else if (strcmp(cmd, "sbl_autobaud_scan") == 0)
    {
        // Common bauds for CC13xx/CC26xx ROM
        const int try_bauds[] = {115200, 921600, 460800, 230400, 57600, 38400, 19200, 9600};
        int found = 0;
        if (sbl_autobaud_scan(dev, try_bauds, sizeof(try_bauds) / sizeof(try_bauds[0]), 500, &found) != 0)
        {
            fprintf(stderr, "Auto-baud scan failed (no ACK at tested bauds).\n");
            rc = 1;
            goto done;
        }
        printf("Auto-baud OK at %d (ACK 0xCC).\n", found);
    }
    else if (strcmp(cmd, "sbl_ping") == 0)
    {
        if (sbl_ping(fd, 500) != 0)
        {
            fprintf(stderr, "PING failed.\n");
            rc = 1;
            goto done;
        }
        printf("PING OK.\n");
    }
    else if (strcmp(cmd, "sbl_status") == 0)
    {
        uint8_t st = 0xFF;
        if (sbl_get_status(fd, 500, &st) != 0)
        {
            fprintf(stderr, "GET_STATUS failed.\n");
            rc = 1;
            goto done;
        }
        printf("STATUS: 0x%02X\n", st);
    }
    else if (strcmp(cmd, "sbl_chipid") == 0)
    {
        uint32_t id = 0;
        if (sbl_get_chip_id(fd, 500, &id) != 0)
        {
            fprintf(stderr, "GET_CHIP_ID failed.\n");
            rc = 1;
            goto done;
        }
        printf("CHIP ID: 0x%08X\n", id);
    }
    else if (strcmp(cmd, "sbl_reset") == 0)
    {
        if (sbl_reset(fd, 500) != 0)
        {
            fprintf(stderr, "RESET failed.\n");
            rc = 1;
            goto done;
        }
        printf("RESET OK.\n");
    }
    else if (strcmp(cmd, "sbl_download") == 0)
    {
        if (argc != 6)
        {
            usage(argv[0]);
            rc = 1;
            goto done;
        }
        uint32_t addr = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t len = (uint32_t)strtoul(argv[5], NULL, 0);
        if (sbl_download(fd, addr, len, 1000) != 0)
        {
            fprintf(stderr, "DOWNLOAD failed\n");
            rc = 1;
            goto done;
        }

        uint8_t status;
        sbl_get_status(fd, 500, &status);
        if(status == 0x40)
        {
            printf("Download accepted: addr=0x%08X len=%u\n", addr, len);
        }
        else
        {
            printf("Download command returned an error: 0x%02X\n", status);
        }
    }
    else if (strcmp(cmd, "sbl_erase") == 0)
    {
        if (argc != 5)
        {
            usage(argv[0]);
            rc = 1;
            goto done;
        }
        uint32_t addr = (uint32_t)strtoul(argv[4], NULL, 0);
        if (sbl_sector_erase(fd, addr, 2000) != 0)
        {
            fprintf(stderr, "SECTOR_ERASE failed\n");
            rc = 1;
            goto done;
        }

        uint8_t status;
        sbl_get_status(fd, 500, &status);
        if(status == 0x40)
        {
            printf("Erase OK at 0x%08X\n", addr);
        }
        else
        {
            printf("Erase command returned an error: 0x%02X\n", status);
        }
    }
    else if (strcmp(cmd, "sbl_full_erase") == 0)
    {
        if (argc != 6)
        {
            usage(argv[0]);
            rc = 1;
            goto done;
        }
        uint32_t flash_size = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t page_size = (uint32_t)strtoul(argv[5], NULL, 0);

        uint32_t last_page_start = flash_size - page_size; // CCFG page
        for (uint32_t a = 0; a < last_page_start; a += page_size)
        {
            if (sbl_sector_erase(fd, a, 2000) != 0)
            {
                fprintf(stderr, "Erase failed at 0x%08X\n", a);
                rc = 1;
                goto done;
            }

            uint8_t status;
            sbl_get_status(fd, 500, &status);
            if(status == 0x40)
            {
                printf("Erase OK at 0x%08X\n", a);
            }
            else
            {
                fprintf(stderr, "Erase failed at 0x%08X with error: 0x%02X\n", a, status);
                rc = 1;
                goto done;
            }
        }
        printf("Full erase done up to (but not including) CCFG at 0x%08X\n", last_page_start);
    }
    else if (strcmp(cmd, "sbl_send_data") == 0)
    {
        if (argc < 5)
        {
            usage(argv[0]);
            rc = 1;
            goto done;
        }
        size_t count = (size_t)(argc - 4);
        if (count > 252)
        {
            fprintf(stderr, "Too many bytes (max 252)\n");
            rc = 1;
            goto done;
        }

        uint8_t buf[252];
        for (size_t i = 0; i < count; ++i)
        {
            unsigned v = (unsigned)strtoul(argv[4 + i], NULL, 0);
            if (v > 255)
            {
                fprintf(stderr, "Invalid byte: %s\n", argv[4 + i]);
                rc = 1;
                goto done;
            }
            buf[i] = (uint8_t)v;
        }

        if (sbl_send_data(fd, buf, count, 1000) != 0)
        {
            fprintf(stderr, "SEND_DATA failed\n");
            rc = 1;
            goto done;
        }
        
        printf("Sent %zu data bytes OK.\n", count);
    }
    else if(strcmp(cmd, "sbl_crc") == 0)
    {
        if(argc != 7){
            usage(argv[0]);
            rc = 1;
            goto done;
        }

        uint32_t address = (uint32_t)strtoul(argv[4], NULL, 0);
        uint32_t len = (uint32_t)strtoul(argv[5], NULL, 0);
        uint32_t repeat = (uint32_t)strtoul(argv[6], NULL, 0);
        uint32_t crc_out = 0;

        if(sbl_crc32(fd, address, len, repeat, 5000, &crc_out) != 0){
            fprintf(stderr, "GETTING CRC FAILED\n");
            rc = 1;
            goto done;
        }

        uint8_t status;
        sbl_get_status(fd, 500, &status);
        if(status == 0x40)
        {
            printf("Sending OK at 0x%08X\n", address);
        }
        else
        {
            //TODO maybe add retry mechanism?
            fprintf(stderr, "Sending failed at 0x%08X with error: 0x%02X\n", address, status);
            rc = 1;
            goto done;
        }
        printf("CRC OK. Received CRC: 0x%08X\n", crc_out);
    }
    else if(strcmp(cmd, "sbl_program") == 0)
    {
        if(argc != 8){
            usage(argv[0]);
            rc = 1;
            goto done;
        }
        
        size_t len;
        unsigned char* image = load_bin(argv[4], &len);

        uint32_t address = (uint32_t)strtoul(argv[5], NULL, 0);
        uint32_t flash_size = (uint32_t)strtoul(argv[6], NULL, 0);
        uint32_t page_size = (uint32_t)strtoul(argv[7], NULL, 0);
        
        sbl_program_binary(fd, flash_size, page_size, image, len, address);
    }
    else
    {
        usage(argv[0]);
        rc = 1;
    }

done:
    serial_close(fd);
    return rc;
}
