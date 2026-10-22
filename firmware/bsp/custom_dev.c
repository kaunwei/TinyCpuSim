#include "custom_dev.h"
void custom_accel_init(uint32_t latency) {
    CUSTOM0->LATENCY = latency;
}
uint32_t custom_accel_add(uint32_t a, uint32_t b) {
    CUSTOM0->REG_A = a;
    CUSTOM0->REG_B = b;
    CUSTOM0->CMD = 1U;
    while (CUSTOM0->STATUS & CUSTOM_STATUS_BUSY) {}
    return CUSTOM0->RESULT;
}
