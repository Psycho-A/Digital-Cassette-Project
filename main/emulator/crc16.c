// ------------------------
// CRC-16 commons
// ------------------------
#include "crc16.h"

// классическая CRC-16-IBM
uint16_t crc16(const uint8_t* data, int len)
{
    uint16_t crc = 0xFFFF;
    int i, j;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

// классический CCITT
uint16_t crc16_ccitt(const uint8_t* data, int len)
{
    uint16_t crc = 0xFFFF;
    int i;

    while (len--) {
        crc ^= (*data++) << 8;
        for (i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

// incremental CRC-16 (полином 0xA001)
uint16_t crc16_update(uint16_t crc, const uint8_t *data, int len)
{
    int i, j;
    for (i = 0; i < len; ++i) {
        crc ^= data[i];
        for (j = 0; j < 8; ++j) {
            if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}
