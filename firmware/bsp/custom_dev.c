#include "custom_dev.h"
uint32_t custom_accel_add(uint32_t a, uint32_t b) {
    CUSTOM0->REG_A = a;
    CUSTOM0->REG_B = b;
    CUSTOM0->CMD = 1;
    return CUSTOM0->RESULT;
}
