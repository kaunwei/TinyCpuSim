#include "shell.h"
#include "uart.h"
#include "../template/fw_config.h"

static int fw_str_equals(const char *a, const char *b) {
    while (*a && *b && (*a == *b)) {
        a++;
        b++;
    }
    return (*a == *b) ? 1 : 0;
}

static int fw_str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

static uint32_t fw_parse_u32(const char *str) {
    uint32_t val = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10u + (uint32_t)(*str - '0');
        str++;
    }
    return val;
}

static void fw_print_u32(uint32_t val) {
    if (val == 0) {
        uart_putc('0');
        return;
    }
    static const uint32_t powers10[10] = {
        1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
        10000u, 1000u, 100u, 10u, 1u
    };
    int started = 0;
    for (int p = 0; p < 10; p++) {
        uint32_t div = powers10[p];
        char digit = '0';
        while (val >= div) {
            val -= div;
            digit++;
        }
        if (digit != '0' || started || p == 9) {
            uart_putc(digit);
            started = 1;
        }
    }
}

void fw_shell_init(fw_shell_t *shell) {
    if (!shell) return;
    shell->line_len = 0;
    shell->cmd_count = 0;
    shell->overflow_count = 0;
    shell->line_buf[0] = 0;
}

void fw_shell_exec_line(fw_shell_t *shell, const char *line) {
    if (!shell || !line) return;
    shell->cmd_count++;

    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;

    if (fw_str_equals(line, "ping")) {
        uart_puts("PONG\r\n");
    } else if (fw_str_equals(line, "help")) {
        uart_puts("CMDS: ping, help, status, compute <N>, selfcheck\r\n");
    } else if (fw_str_equals(line, "status")) {
        uart_puts("STATUS: OK\r\n");
    } else if (fw_str_equals(line, "selfcheck")) {
        uint32_t expected = FW_SELF_CHECK_EXPECTED;
        uint32_t computed = FW_SELF_CHECK_INITIAL_COUNTER + FW_SELF_CHECK_INCREMENT;
        if (computed == expected) {
            uart_puts("SELFCHECK: PASS\r\n");
        } else {
            uart_puts("SELFCHECK: FAIL\r\n");
        }
    } else if (fw_str_starts_with(line, "compute")) {
        const char *arg = line + 7;
        while (*arg == ' ') arg++;
        uint32_t n = fw_parse_u32(arg);
        uint32_t sum = 0;
        for (uint32_t i = 1; i <= n; ++i) {
            sum += i * i;
        }
        uart_puts("RESULT: ");
        fw_print_u32(sum);
        uart_puts("\r\n");
    } else {
        uart_puts("ERR: unknown cmd\r\n");
    }
}

void fw_shell_poll(fw_shell_t *shell) {
    if (!shell) return;
    char c = 0;
    while (uart_getc_nonblocking(&c)) {
        if (c == '\r' || c == '\n') {
            if (shell->line_len > 0) {
                shell->line_buf[shell->line_len] = 0;
                fw_shell_exec_line(shell, shell->line_buf);
                shell->line_len = 0;
            }
        } else if (c == '\b' || c == 0x7f) {
            if (shell->line_len > 0) shell->line_len--;
        } else {
            if (shell->line_len < (FW_SHELL_LINE_MAX - 1)) {
                shell->line_buf[shell->line_len++] = c;
            } else {
                shell->overflow_count++;
                uart_puts("ERR: line overflow\r\n");
                shell->line_len = 0;
            }
        }
    }
}

void fw_shell_service(void *context) {
    fw_shell_poll((fw_shell_t *)context);
}
