/* frame.c */
/* -------------------------
 * Payload packing (MSB/LSB)
 * Payload parse & unpacking
 * -------------------------
 */
#include <stdlib.h>
#include <string.h>
#include "frame.h"
#include "crc16.h"

/* Доп. биты ёмкости фрейма (изменяемое) */
uint8_t SERVICE_BITS = SERVICE_FULL;  // с sync_word

/* Хелперы НОД и НОК для вычисления ёмкостей */
uint32_t gcd(uint32_t a, uint32_t b) {
    while (b) { uint32_t t = b; b = a % b; a = t; }
    return a;
}
uint32_t lcm(uint32_t a, uint32_t b) {
    return a / gcd(a, b) * b;
}


/* build_frame_data: allocates msb_buf and lsb_buf,
   splits every byte rank to 4:4 MSB/LSB nibbles,
   and then serializes these nibbles to frame bit layout */
int build_frame_data(FrameDesc *desc, uint8_t *payload, uint8_t *bitbuf, uint32_t bitbuf_len)
{
    int rc = 0;
    int i, b, bit;
    uint8_t  hdr_bytes[6];
    uint32_t payload_len;
    uint16_t payload_half1;
    uint16_t payload_half2;
    uint16_t payload_active;
    uint32_t pos = 0;

    if (!desc || !payload) return -1;
    if (!bitbuf || bitbuf_len <= 0) return -1;

    /* allocate buffers */
    payload_len = desc->payload_bytes;
    desc->sync.sync_word = SYNC_MAGIC;
    desc->msb_buf = (uint8_t*)malloc(payload_len);
    desc->lsb_buf = (uint8_t*)malloc(payload_len);
    if (!desc->msb_buf || !desc->lsb_buf) { rc = -1; goto fail; }

    /* determine bytes number to read */
    payload_active = desc->hdr.payload_used;  // default: data only
    if (desc->payload_extra > desc->hdr.payload_used) {
        payload_active = desc->payload_extra;  // metadata found, expand
    }

    /* encode payload with chosen way */
    if (!desc->ofdm_encoding) {
        /* just split payload into 2 parts (for ASK) */
        payload_half1 = (payload_active + 1) / 2;
        payload_half2 = (payload_active + 0) / 2;
        memcpy(desc->msb_buf, payload, payload_half1);
        memcpy(desc->lsb_buf, payload +payload_half1, payload_half2);
    } else {
        /* pack payload to msb/lsb planes (for OFDM) */
        pack_msb_lsb_4_4(payload, payload_len, desc->msb_buf, desc->lsb_buf);
    }

    /* fill header for CRC (big-endian) */
    hdr_bytes[0] =  desc->hdr.symbols_bits;
    hdr_bytes[1] =  desc->hdr.payload_class;
    hdr_bytes[2] = (desc->hdr.frame_number >> 8) & 0xFF;
    hdr_bytes[3] =  desc->hdr.frame_number & 0xFF;
    hdr_bytes[4] = (desc->hdr.payload_used >> 8) & 0xFF;
    hdr_bytes[5] =  desc->hdr.payload_used & 0xFF;

    /* compute CRC for header bits (6 bytes) */
    desc->crc_hdr = crc16(hdr_bytes, 6);

    /* compute CRC over MSB and LSB data planes */
    desc->crc_msb = crc16(desc->msb_buf, desc->ofdm_encoding ? payload_active : payload_half1);
    desc->crc_lsb = crc16(desc->lsb_buf, desc->ofdm_encoding ? payload_active : payload_half2);

    /* serialize data to bits, layout:
       - preamble bits (computable)
       - symbols_bits + payload_class (2*8 bits)
       - frame_number + payload_used (2*16 bits, big-endian)
       - crc_hdr + crc_msb + crc_lsb (3*16 bits, computed above)
       - msb plane + lsb plane (for bits 3..0 and 7..4)
       - padding zeros up to $capacity_bits
    */
    /* emit preamble bits */
    emit_preamble_bits(&desc->sync, bitbuf, &pos);

    /* helper to write byte as bits MSB-first */
    #define WRITE_BYTE_BITS(byte) \
        for (b=7;b>=0;b--) { bit = ((byte) >> b) & 1; bitbuf[pos++] = (uint8_t)bit; }

    /* sync-pattern (4 bytes) */
    if (SERVICE_BITS == SERVICE_FULL) {
    WRITE_BYTE_BITS((desc->sync.sync_word >> 24) & 0xFF);
    WRITE_BYTE_BITS((desc->sync.sync_word >> 16) & 0xFF);
    WRITE_BYTE_BITS((desc->sync.sync_word >> 8) & 0xFF);
    WRITE_BYTE_BITS( desc->sync.sync_word & 0xFF); }

    /* header (6 bytes) */
    WRITE_BYTE_BITS( desc->hdr.symbols_bits & 0xFF);
    WRITE_BYTE_BITS( desc->hdr.payload_class & 0xFF);
    WRITE_BYTE_BITS((desc->hdr.frame_number >> 8) & 0xFF);
    WRITE_BYTE_BITS( desc->hdr.frame_number & 0xFF);
    WRITE_BYTE_BITS((desc->hdr.payload_used >> 8) & 0xFF);
    WRITE_BYTE_BITS( desc->hdr.payload_used & 0xFF);

    /* CRC for hdr, msb and lsb (6 bytes) */
    WRITE_BYTE_BITS((desc->crc_hdr >> 8) & 0xFF);
    WRITE_BYTE_BITS( desc->crc_hdr & 0xFF);
    WRITE_BYTE_BITS((desc->crc_msb >> 8) & 0xFF);
    WRITE_BYTE_BITS( desc->crc_msb & 0xFF);
    WRITE_BYTE_BITS((desc->crc_lsb >> 8) & 0xFF);
    WRITE_BYTE_BITS( desc->crc_lsb & 0xFF);

    /* write encoded payload bits */
    if (!desc->ofdm_encoding) {
        /* splitted payload halves (ASK) */
        for (i = 0; i < payload_half1; i++) WRITE_BYTE_BITS(desc->msb_buf[i]);
        for (i = 0; i < payload_half2; i++) WRITE_BYTE_BITS(desc->lsb_buf[i]);
    } else {
        /* msb planes: bits 3,2,1,0 */
        for (bit = 3; bit >= 0; --bit) {
            for (i = 0; i < (int)payload_active; ++i) {
                b = (desc->msb_buf[i] >> bit) & 1; bitbuf[pos++] = (uint8_t)b;
            }
        }
        /* lsb planes: bits 7,6,5,4 */
        for (bit = 3; bit >= 0; --bit) {
            for (i = 0; i < (int)payload_active; ++i) {
                b = (desc->lsb_buf[i] >> bit) & 1; bitbuf[pos++] = (uint8_t)b;
            }
        }
    }
    /* padding rest with zeros */
    while (pos < bitbuf_len) bitbuf[pos++] = 0;

fail:
    /* освобождаем frame-owned буферы */
    if (desc->msb_buf) free(desc->msb_buf);
    if (desc->lsb_buf) free(desc->lsb_buf);

    return rc;
}

/* разбивает байты payload на две 4:4-тетрады:
   - lsb_buf: младшие 4 бита (b3..b0)
   - msb_buf: старшие 4 бита (b7..b4) */
static inline void pack_msb_lsb_4_4(uint8_t *src, uint32_t len, uint8_t *msb_buf, uint8_t *lsb_buf)
{
    uint32_t i; uint8_t b;
    for (i = 0; i < len; ++i) {
        b = src[i];
        lsb_buf[i] = b & 0x0F;         // b3..b0
        msb_buf[i] = (b >> 4) & 0x0F;  // b7..b4
    }
}

/* private helper to emit preamble bits */
static inline void emit_preamble_bits(const FrameSync *sync, uint8_t *bitbuf, uint32_t *pos)
{
    uint32_t i; uint8_t bit;
    if (!sync || sync->preamble_len_bits == 0) return;

    for (i = 0; i < sync->preamble_len_bits; ++i) {
        switch (sync->preamble_pattern) {
            default:
            case PREAMBLE_PATTERN_ALT:
                /* 1 0 1 0 */
                bit = (i & 1) ? 1 : 0;
                break;
            case PREAMBLE_PATTERN_PN4:
                /* 1 0 0 1 */
                bit = ((i & 3) == 0 || (i & 3) == 3) ? 1 : 0;
                break;
        }
        bitbuf[(*pos)++] = bit;
    }
}


/* Функция декодирования заголовка фрейма с проверкой CRC */
int parse_frame_header(FrameDesc *desc, uint8_t *bitbuf, uint8_t *payload_buf)
{
    uint32_t pos = 0;
    uint16_t payload_len;
    uint8_t  hdr_bytes[6];
    uint16_t calc_crc_hdr;

    /* значения по умолчанию */
    desc->crc_hdr_valid = desc->frame_valid = 0;
    desc->crc_msb_valid = desc->crc_lsb_valid = 0;
    desc->payload_extra = 0; // маркер метаданных
    payload_len = desc->payload_bytes;

    if (!desc || !bitbuf || !payload_buf) return -1;
    if (payload_len <= 0) return -1;

    /* читаем sync-слово (4 байта = 32 бит) */
    if (SERVICE_BITS == SERVICE_FULL) {
    desc->sync.sync_word = (uint32_t)read_byte_bits(bitbuf, &pos, 32); }

    /* читаем заголовок (6 байт = 48 бит) */
    desc->hdr.symbols_bits  = (uint8_t )read_byte_bits(bitbuf, &pos, 8);
    desc->hdr.payload_class = (uint8_t )read_byte_bits(bitbuf, &pos, 8);
    desc->hdr.frame_number  = (uint16_t)read_byte_bits(bitbuf, &pos, 16);
    desc->hdr.payload_used  = (uint16_t)read_byte_bits(bitbuf, &pos, 16);

    /* читаем группы CRC (3 по 2 байта = 48 бит) */
    desc->crc_hdr = (uint16_t)read_byte_bits(bitbuf, &pos, 16);
    desc->crc_msb = (uint16_t)read_byte_bits(bitbuf, &pos, 16);
    desc->crc_lsb = (uint16_t)read_byte_bits(bitbuf, &pos, 16);

    /* проверим выход payload_used за буфер */
    if (desc->hdr.payload_used > PAYLOAD_MAX) return -1;

    /* проверяем CRC заголовка (сразу) */
    hdr_bytes[0] =  desc->hdr.symbols_bits;
    hdr_bytes[1] =  desc->hdr.payload_class;
    hdr_bytes[2] = (desc->hdr.frame_number >> 8) & 0xFF;
    hdr_bytes[3] =  desc->hdr.frame_number & 0xFF;
    hdr_bytes[4] = (desc->hdr.payload_used >> 8) & 0xFF;
    hdr_bytes[5] =  desc->hdr.payload_used & 0xFF;

    calc_crc_hdr = crc16(hdr_bytes, 6);
    desc->crc_hdr_valid = (calc_crc_hdr == desc->crc_hdr);
    if (!desc->crc_hdr_valid) return -1;

    return 0;
}

/* Функция декодирования данных фрейма с проверкой CRC */
int parse_frame_payload(FrameDesc *desc, uint8_t *bitbuf, uint8_t *payload_buf)
{
    int rc = 0, plane = 0;
    uint32_t i = 0, pos = 0;
    uint16_t payload_len;
    uint16_t payload_half1;
    uint16_t payload_half2;
    uint16_t payload_active;
    uint8_t  msb_nib, lsb_nib;
    uint16_t calc_crc_msb;
    uint16_t calc_crc_lsb;

    if (!desc || !bitbuf || !payload_buf) return -1;
    if (!desc->crc_hdr_valid) return -1;
    if (desc->payload_bytes <= 0) return -1;

    /* выделяем MSB/LSB-буферы */
    payload_len = desc->payload_bytes;
    desc->msb_buf = (uint8_t*)malloc(4 * payload_len);
    desc->lsb_buf = (uint8_t*)malloc(4 * payload_len);
    if (!desc->msb_buf || !desc->lsb_buf) { rc = -1; goto fail; }

    /* определим кол-во байт для чтения */
    payload_active = desc->hdr.payload_used;  // только данные
    if (desc->payload_extra > desc->hdr.payload_used) {
        payload_active = desc->payload_extra;  // найдены метаданные, расширим
    }

    /* пропускаем биты заголовка */
    pos = SERVICE_BITS;  // уже прочитан в parse_frame_header

    /* декодирование бит и сборка данных */
    if (!desc->ofdm_encoding) {
        /* читаем биты из двух половин (ASK) */
        payload_half1 = (payload_active + 1) / 2;
        payload_half2 = (payload_active + 0) / 2;
        for (i = 0; i < payload_half1; i++) {
            desc->msb_buf[i] = (uint8_t)read_byte_bits(bitbuf, &pos, 8);
        }
        for (i = 0; i < payload_half2; i++) {
            desc->lsb_buf[i] = (uint8_t)read_byte_bits(bitbuf, &pos, 8);
        }
        /* сборка payload_buf из msb/lsb_buf */
        for (i = 0; i < payload_half1; i++) payload_buf[i] = desc->msb_buf[i];
        for (i = 0; i < payload_half2; i++) payload_buf[payload_half1 + i] = desc->lsb_buf[i];

    } else {
        /* читаем плоскости для msb_buf (старшие биты) */
        for (plane = 3; plane >= 0; plane--) {
            for (i = 0; i < payload_active; i++) {
                desc->msb_buf[plane * payload_len + i] = bitbuf[pos++];
            }
        }
        /* читаем плоскости для lsb_buf (младшие биты) */
        for (plane = 3; plane >= 0; plane--) {
            for (i = 0; i < payload_active; i++) {
                desc->lsb_buf[plane * payload_len + i] = bitbuf[pos++];
            }
        }
        /* собираем биты из плоскостей в нибблы (4:4) */
        for (i = 0; i < payload_active; i++) {
            msb_nib = (desc->msb_buf[3 * payload_len + i] << 3) |
                      (desc->msb_buf[2 * payload_len + i] << 2) |
                      (desc->msb_buf[1 * payload_len + i] << 1) |
                      (desc->msb_buf[0 * payload_len + i] << 0);
            lsb_nib = (desc->lsb_buf[3 * payload_len + i] << 3) |
                      (desc->lsb_buf[2 * payload_len + i] << 2) |
                      (desc->lsb_buf[1 * payload_len + i] << 1) |
                      (desc->lsb_buf[0 * payload_len + i] << 0);
            /* перезаписываем буферы нибблами и собираем в байты */
            desc->msb_buf[i] = msb_nib; desc->lsb_buf[i] = lsb_nib;
            payload_buf[i] = (msb_nib << 4) | lsb_nib;
        }
    }

    /* вычисляем CRC для MSB и LSB-плоскостей данных */
    calc_crc_msb = crc16(desc->msb_buf, desc->ofdm_encoding ? payload_active : payload_half1);
    calc_crc_lsb = crc16(desc->lsb_buf, desc->ofdm_encoding ? payload_active : payload_half2);

    /* определяем, что делать с битым фреймом */
    desc->crc_msb_valid = (calc_crc_msb == desc->crc_msb);
    desc->crc_lsb_valid = (calc_crc_lsb == desc->crc_lsb);
    desc->frame_valid   = (desc->crc_msb_valid && desc->crc_lsb_valid);

fail:
    /* освобождаем frame-owned буферы */
    if (desc->msb_buf) free(desc->msb_buf);
    if (desc->lsb_buf) free(desc->lsb_buf);

    return rc;
}

/* Вспомогательная функция для чтения битов из буфера (big-endian) */
static inline uint32_t read_byte_bits(uint8_t *bitbuf, uint32_t *pos, uint32_t nbits)
{
    uint32_t i, val = 0;
    for (i = 0; i < nbits; i++) {
        val = (val << 1) | bitbuf[(*pos)++];
    }
    return val;
}
