#pragma once

#include <stdint.h>
#include <stddef.h>

#define FW_SHELL_LINE_MAX 64u

typedef struct {
    char line_buf[FW_SHELL_LINE_MAX];
    uint32_t line_len;
    uint32_t cmd_count;
    uint32_t overflow_count;
} fw_shell_t;

#ifdef __cplusplus
extern "C" {
#endif

void fw_shell_init(fw_shell_t *shell);
void fw_shell_exec_line(fw_shell_t *shell, const char *line);
void fw_shell_poll(fw_shell_t *shell);
void fw_shell_service(void *context);

#ifdef __cplusplus
}
#endif
