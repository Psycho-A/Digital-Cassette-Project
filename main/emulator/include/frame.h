/* frame.h */
/* ----------------
 * Структура фрейма
 * ----------------
 * Active terminology:
 * - bit      : logical payload bit (protocol/buffer level)
 * - symbol   : group of ASK periods or OFDM symbol (frequency domain)
 * - preamble : sync point of each frame start (level and frequency domain)
 * - frame    : group of ASK/OFDM symbols with preamble (data domain)
 *
 * Suggested sync-words:
 * - 0xDEADBEEF (Standard)
 * - 0xFA6D48C7 (Suboptimal 32)
 * - 0x121D12E2 (Golay 32)
 */
#ifndef FRAME_H
#define FRAME_H
#include <stdio.h>
#include "stdint_compat.h"

/* Классы данных */
#include "frame_types.h"

#define SYNC_MAGIC            0xDEADBEEF
#define SYNC_BITS             32
#define HEADER_BITS           48  // see FrameHeader
#define CRC_BITS              48  // 3*16 bit fields
#define SERVICE_FULL          (SYNC_BITS +HEADER_BITS +CRC_BITS)  // 16 bytes

#define PAYLOAD_MIN           32    // min bytes cap in frame (for payload_bytes)
#define PAYLOAD_MAX           4096  // max bytes cap in frame (for RX payload_buf)
#define BITSCAP_MAX           (SERVICE_FULL + PAYLOAD_MAX * 8)  // for RX bitbuf

#define PREAMBLE_PATTERN_ALT  0  // 101010...
#define PREAMBLE_PATTERN_PN4  1  // 1001...

/* Доп. биты ёмкости фрейма */
extern uint8_t SERVICE_BITS;     // задаётся во frame.c для видимости в функциях


typedef struct {
    uint32_t sync_word;          // frame auto-correlating pattern (OFDM only)
    uint32_t preamble_len_bits;  // length in bits (not samples yet!)
    uint8_t  preamble_pattern;   // preamble pattern to emit (enum)
} FrameSync;

typedef struct {
    uint8_t  symbols_bits;       // OFDM symbols (1-16) or m-ASK symbol_bits (in 0x00) +Gray mapping (in 0x10)
    uint8_t  payload_class;      // data class (0..70), extends to sub-types
    uint16_t frame_number;       // порядковый номер фрейма в текущем файле
    uint16_t payload_used;       // реально использованные байты (<= payload_bytes)
} FrameHeader;

/* Frame descriptor,
   used for TX/RX to carry buffers */
typedef struct {
    FrameSync sync;
    FrameHeader hdr;
    uint16_t crc_hdr;
    uint16_t crc_msb;
    uint16_t crc_lsb;
    uint8_t *msb_buf;            // старшие 4 бита (b7..b4) каждого байта
    uint8_t *lsb_buf;            // младшие 4 бита (b3..b0) каждого байта
    uint16_t payload_bytes;      // максимальный размер полезных данных во фрейме
    uint16_t payload_extra;      // кол-во байт метаданных после payload_used (если есть)
    int  ofdm_encoding;          // 1 = кодируем bit-плейны, 0 = делим данные на 2 половины

    // диагностика
    int  frame_valid;            // валидность всего фрейма (header + оба CRC верны)
    int  crc_hdr_valid;          // валидность заголовка (если битый, фрейм отброшен)
    int  crc_msb_valid;          // валидность MSB-зоны данных (для применения политик)
    int  crc_lsb_valid;          // валидность LSB-зоны данных (для применения политик)
} FrameDesc;


/* helpers to find GCD and LCM for frame capacity bits */
uint32_t gcd(uint32_t a, uint32_t b);  // наиб. общий делитель
uint32_t lcm(uint32_t a, uint32_t b);  // наим. общее кратное

/* build msb/lsb buffers and CRCs, then
   serialize frame into preallocated bit buffer (bits as bytes 0/1).
   buffer must be at least frame_capacity_bits(...) bytes long.
   returns 0 if ok, or -1 on error */
int build_frame_data(FrameDesc *desc, uint8_t *payload, uint8_t *bitbuf, uint32_t bitbuf_len);

/* helpers to emit preamble and pack payload MSB:LSB planes */
void pack_msb_lsb_4_4(uint8_t *src, uint32_t len, uint8_t *msb_buf, uint8_t *lsb_buf);
void emit_preamble_bits(const FrameSync *sync, uint8_t *bitbuf, uint32_t *pos);

/* decode demodulated frame header + payload and check CRCs */
int parse_frame_header(FrameDesc *desc, uint8_t *bitbuf, uint8_t *payload_buf);
int parse_frame_payload(FrameDesc *desc, uint8_t *bitbuf, uint8_t *payload_buf);

/* helpers to check header field ranges read from bit-buffer */
uint32_t read_byte_bits(uint8_t *bitbuf, uint32_t *pos, uint32_t nbits);


#endif