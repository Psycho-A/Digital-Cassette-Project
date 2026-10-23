#ifndef CRC16_H
#define CRC16_H

#include "stdint_compat.h"

uint16_t crc16(const uint8_t* data, int len);
uint16_t crc16_ccitt(const uint8_t* data, int len);
uint16_t crc16_update(uint16_t crc, const uint8_t *data, int len);

#endif
