#include "timer.h"
void timer_init(uint32_t load) {
    TIMER0->LOAD = load;
    TIMER0->CONTROL = 0x07; // Enable | Periodic | IRQ_EN
}
