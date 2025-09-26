#ifndef SBL_H
#define SBL_H

#include <stdint.h>
#include <sys/types.h>

// ACK/NACK per TI SBL
#define SBL_ACK 0xCC
#define SBL_NACK 0x33

// Bootloader command IDs (CC13xx/CC26xx/CC2538 family)
enum
{
    CMD_PING = 0x20,
    CMD_DOWNLOAD = 0x21,
    CMD_GET_STATUS = 0x23,
    CMD_SEND_DATA = 0x24,
    CMD_RESET = 0x25,
    CMD_SECTOR_ERASE = 0x26,
    CMD_CRC32 = 0x27,
    CMD_GET_CHIP_ID = 0x28
};

// GET_STATUS return codes (subset)
enum
{
    COMMAND_RET_SUCCESS = 0x40,
    COMMAND_RET_UNKNOWN_CMD = 0x41,
    COMMAND_RET_INVALID_CMD = 0x42,
    COMMAND_RET_INVALID_ADR = 0x43,
    COMMAND_RET_FLASH_FAIL = 0x44
};

// Initialize ROM bootloader UART comms: send 0x55 0x55 and expect ACK.
// Returns 0 on success, -1 on error/timeout.
int sbl_autobaud(int fd, int timeout_ms);

// Try multiple bauds by opening the port for each baud.
// Returns 0 on success and writes the working baud to *baud_ok.
// Returns -1 if none worked.
int sbl_autobaud_scan(const char *dev_path,
                      const int *bauds, size_t n_bauds,
                      int timeout_ms, int *baud_ok);

// Send a generic SBL packet: data[0] must be the CMD byte.
// Returns 0 on ACK, -1 on NACK/error; optionally reads a response payload into out.
int sbl_send_cmd(int fd, const uint8_t *data, size_t len,
                 uint8_t *out, size_t out_max, int timeout_ms);

// SBL functions
int sbl_ping(int fd, int timeout_ms);
int sbl_get_status(int fd, int timeout_ms, uint8_t *status_out);
int sbl_get_chip_id(int fd, int timeout_ms, uint32_t *chip_id_out);
int sbl_reset(int fd, int timeout_ms);
int sbl_download(int fd, uint32_t addr, uint32_t total_len, int timeout_ms);
int sbl_sector_erase(int fd, uint32_t addr, int timeout_ms);
int sbl_send_data(int fd, const uint8_t *chunk, size_t n, int timeout_ms);
int sbl_crc32(int fd, uint32_t addr, uint32_t len, uint32_t repeat, int timeout_ms, uint32_t *crc_out);
int sbl_program_binary(int fd,
                       uint32_t flash_size, uint32_t page_size,
                       const uint8_t *image, size_t image_len,
                       uint32_t base_addr);
#endif
