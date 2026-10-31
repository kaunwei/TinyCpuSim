#include "gpio.h"

void gpio_init(uint32_t moder_mask, uint32_t debounce_ticks) {
    GPIO0->MODER = moder_mask;
    GPIO0->DEBOUNCE = debounce_ticks;
}

void gpio_set_pins(uint16_t pin_mask) {
    GPIO0->BSRR = (uint32_t)pin_mask;
}

void gpio_reset_pins(uint16_t pin_mask) {
    GPIO0->BSRR = ((uint32_t)pin_mask) << 16;
}

uint32_t gpio_read_pins(void) {
    while (GPIO0->BUSY) {}
    return GPIO0->IDR;
}
