#pragma once
#include "platform.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void uart_init(uint32_t clkdiv);
void uart_putc(char c);
void uart_puts(const char* s);
int uart_has_rx(void);
int uart_getc_nonblocking(char *out_c);

#ifdef __cplusplus
}
#endif
