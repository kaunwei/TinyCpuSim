#include <stdint.h>

volatile uint32_t initialized_value = 0x13579BDFu;
volatile uint32_t zero_value;
volatile uint32_t acceptance_result;

extern uint32_t exercise_isa_families(uint32_t value);

int main(void) {
    if (initialized_value != 0x13579BDFu || zero_value != 0u) {
        acceptance_result = 1u;
        return 1;
    }
    acceptance_result = exercise_isa_families(initialized_value);
    return 0;
}
