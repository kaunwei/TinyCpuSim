#include "crc.h"
uint32_t crc_calc(uint32_t val) {
    CRC0->CR = 1; // reset
    CRC0->DR = val;
    return CRC0->DR;
}
