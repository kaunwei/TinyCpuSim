#include <stdint.h>
#include "fw_config.h"

volatile uint32_t g_counter = 0;

int main(void) {
    g_counter = 42;
    while (1) {
        g_counter++;
    }
    return 0;
}
