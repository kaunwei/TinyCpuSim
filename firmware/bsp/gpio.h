#pragma once
#include "platform.h"

void gpio_init(uint32_t moder_mask, uint32_t debounce_ticks);
void gpio_set_pins(uint16_t pin_mask);
void gpio_reset_pins(uint16_t pin_mask);
uint32_t gpio_read_pins(void);
