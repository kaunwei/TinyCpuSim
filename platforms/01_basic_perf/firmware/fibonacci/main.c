#include <stdint.h>

volatile uint32_t g_result = 0;

int main(void) {
    uint32_t a = 0, b = 1;
    for (int i = 0; i < 20; ++i) {
        uint32_t next = a + b;
        a = b;
        b = next;
    }
    g_result = b;
    return (int)g_result;
}
