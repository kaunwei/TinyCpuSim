#include "../../bsp/platform.h"
#include "../../bsp/uart.h"

#define TOTAL_ROUNDS 3
#define BUFFER_SIZE  128

static int read_line(char* buf, int max_len) {
    int idx = 0;
    while (1) {
        char c = 0;
        if (!uart_getc_nonblocking(&c)) {
            continue;
        }

        /* Check for Enter (CR or LF) */
        if (c == '\r' || c == '\n') {
            if (idx == 0) {
                /* Ignore stray leading newline or paired CR/LF from previous round */
                continue;
            }
            uart_puts("\r\n");
            buf[idx] = '\0';
            return idx;
        }

        /* Backspace / Delete handling */
        if (c == '\b' || c == 0x7F) {
            if (idx > 0) {
                idx--;
                uart_puts("\b \b");
            }
            continue;
        }

        /* Printable character */
        if (idx < max_len - 1) {
            buf[idx++] = c;
            uart_putc(c); /* Real-time echo back to user */
        }
    }
}

int main(void) {
    uart_init(1U);

    uart_puts("\r\n============================================================\r\n");
    uart_puts("   TinyCpuSim UART Interactive Echo Demo (3 Rounds)         \r\n");
    uart_puts("   Type whatever you want, and the firmware echoes it back! \r\n");
    uart_puts("============================================================\r\n\r\n");

    char buffer[BUFFER_SIZE];

    for (int round = 1; round <= TOTAL_ROUNDS; round++) {
        uart_puts("[Round ");
        uart_putc((char)('0' + round));
        uart_puts("/");
        uart_putc((char)('0' + TOTAL_ROUNDS));
        uart_puts("] Enter message: ");

        read_line(buffer, BUFFER_SIZE);

        uart_puts("[Echo  ");
        uart_putc((char)('0' + round));
        uart_puts("/");
        uart_putc((char)('0' + TOTAL_ROUNDS));
        uart_puts("] -> ");
        uart_puts(buffer);
        uart_puts("\r\n\r\n");
    }

    uart_puts("============================================================\r\n");
    uart_puts(" [Success] All 3 rounds completed! Exiting simulator now.   \r\n");
    uart_puts("============================================================\r\n");

    /* Give host UART output drain time to deliver the final banner */
    for (volatile int delay = 0; delay < 100000; delay++) {}

    return 0;
}
