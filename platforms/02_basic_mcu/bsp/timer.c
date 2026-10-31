#include "timer.h"
void timer_init(uint32_t load, uint32_t psc) {
    TIMER0->PSC = psc;
    TIMER0->LOAD = load;
    TIMER0->CONTROL = 0x07U; // Enable | Periodic | IRQ_EN
}
void timer_delay_ticks(uint32_t load, uint32_t psc) {
    TIMER0->PSC = psc;
    TIMER0->LOAD = load;
    TIMER0->INTCLR = 1U;
    TIMER0->CONTROL = 0x01U; // One-shot | Enable
    while (TIMER0->RIS == 0U) {}
    TIMER0->INTCLR = 1U;
}
