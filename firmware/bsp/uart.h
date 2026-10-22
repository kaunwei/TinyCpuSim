#pragma once
#include "platform.h"
void uart_init(uint32_t clkdiv);
void uart_putc(char c);
void uart_puts(const char* s);
