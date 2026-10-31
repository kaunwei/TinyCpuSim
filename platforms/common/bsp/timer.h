#pragma once
#include "platform.h"
void timer_init(uint32_t load, uint32_t psc);
void timer_delay_ticks(uint32_t load, uint32_t psc);

static inline void timer_clear_irq(void) {
    TIMER0->INTCLR = 1U;
}
