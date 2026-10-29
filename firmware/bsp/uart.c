#include "uart.h"

void uart_init(uint32_t clkdiv) {
    UART0->CLKDIV = clkdiv;
}

void uart_putc(char c) {
    while (!(UART0->STATUS & UART_STATUS_TX_READY)) {}
    UART0->DATA = (uint32_t)c;
    while (UART0->STATUS & UART_STATUS_BUSY) {}
}

void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

int uart_has_rx(void) {
    return (int)(UART0->STATUS & UART_STATUS_RX_VALID);
}

int uart_getc_nonblocking(char *out_c) {
    if (!(UART0->STATUS & UART_STATUS_RX_VALID)) {
        return 0;
    }
    if (out_c) {
        *out_c = (char)(UART0->DATA & 0xFFu);
    }
    return 1;
}
