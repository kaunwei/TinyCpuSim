#include "uart.h"
void uart_putc(char c) { UART0->DATA = (uint32_t)c; }
void uart_puts(const char* s) { while (*s) uart_putc(*s++); }
