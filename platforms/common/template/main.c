#include <stdint.h>
#include "fw_config.h"

volatile uint32_t g_counter = 0;
volatile uint32_t g_self_check_result = 0;
volatile uint32_t g_self_check_status = FW_SELF_CHECK_FAIL;

int main(void) {
    g_counter = FW_SELF_CHECK_INITIAL_COUNTER;
    g_self_check_result = g_counter + FW_SELF_CHECK_INCREMENT;
    g_self_check_status = FW_SELF_CHECK_FAIL;
    if (g_self_check_result == FW_SELF_CHECK_EXPECTED) {
        g_self_check_status = FW_SELF_CHECK_PASS;
    }
    while (1) {
        g_counter++;
    }
    return 0;
}
