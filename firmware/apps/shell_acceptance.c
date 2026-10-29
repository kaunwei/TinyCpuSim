#include "../bsp/platform.h"
#include "../bsp/uart.h"
#include "../bsp/ring_buffer.h"
#include "../bsp/shell.h"

static int test_ring_buffer(void) {
    uint8_t storage[16];
    ring_buffer_t rb;
    ring_buffer_init(&rb, storage, 16);

    if (!ring_buffer_is_empty(&rb)) return 1;
    if (ring_buffer_is_full(&rb)) return 2;
    if (ring_buffer_size(&rb) != 0) return 3;
    if (ring_buffer_capacity(&rb) != 16) return 4;

    for (uint8_t i = 0; i < 16; i++) {
        if (!ring_buffer_put(&rb, (uint8_t)(i + 10))) return 5;
    }
    if (!ring_buffer_is_full(&rb)) return 6;
    if (ring_buffer_is_empty(&rb)) return 7;
    if (ring_buffer_size(&rb) != 16) return 8;

    /* Push when full must return false */
    if (ring_buffer_put(&rb, 99)) return 9;

    /* Read back and verify order */
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t val = 0;
        if (!ring_buffer_get(&rb, &val)) return 10;
        if (val != (uint8_t)(i + 10)) return 11;
    }
    if (!ring_buffer_is_empty(&rb)) return 12;

    /* Read when empty must return false */
    uint8_t dummy = 0;
    if (ring_buffer_get(&rb, &dummy)) return 13;

    /* Test wraparound */
    for (uint8_t i = 0; i < 8; i++) {
        if (!ring_buffer_put(&rb, i)) return 14;
    }
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t val = 0;
        if (!ring_buffer_get(&rb, &val) || val != i) return 15;
    }
    for (uint8_t i = 0; i < 16; i++) {
        if (!ring_buffer_put(&rb, (uint8_t)(i + 100))) return 16;
    }
    if (!ring_buffer_is_full(&rb)) return 17;
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t val = 0;
        if (!ring_buffer_get(&rb, &val) || val != (uint8_t)(i + 100)) return 18;
    }

    return 0;
}

int main(void) {
    uart_init(1U);
    uart_puts("SHELL_START\r\n");

    if (test_ring_buffer() != 0) {
        uart_puts("RING_BUFFER_FAIL\r\n");
        return 1;
    }
    uart_puts("RING_BUFFER_PASS\r\n");

    fw_shell_t shell;
    fw_shell_init(&shell);

    /* Exercise all commands via exec_line */
    fw_shell_exec_line(&shell, "ping");
    fw_shell_exec_line(&shell, "help");
    fw_shell_exec_line(&shell, "status");
    fw_shell_exec_line(&shell, "selfcheck");
    fw_shell_exec_line(&shell, "compute 5");
    fw_shell_exec_line(&shell, "compute 10");
    fw_shell_exec_line(&shell, "unknown_cmd");

    /* Also poll incoming UART RX data if available */
    int idle_count = 0;
    while (idle_count < 1000) {
        if (uart_has_rx()) {
            fw_shell_poll(&shell);
            idle_count = 0;
        } else {
            idle_count++;
        }
    }

    uart_puts("SHELL_ACCEPTANCE_PASS\r\n");
    return 0;
}
