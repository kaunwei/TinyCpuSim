#include <stdint.h>

struct AcceptanceResults {
    uint32_t sp_address;
    uint32_t adr_address;
    uint32_t sxtb;
    uint32_t sxth;
    uint32_t uxtb;
    uint32_t uxth;
    uint32_t rev;
    uint32_t rev16;
    uint32_t revsh;
    uint32_t udiv;
    uint32_t sdiv;
    uint32_t wide_stack;
    uint32_t wide_branch;
};

volatile uint32_t initialized_value = 0x13579BDFu;
volatile uint32_t zero_value;
volatile uint32_t acceptance_result;
volatile struct AcceptanceResults acceptance_results;

extern void exercise_isa_families(uint32_t value, struct AcceptanceResults *results);

int main(void) {
    if (initialized_value != 0x13579BDFu || zero_value != 0u) {
        acceptance_result = 1u;
        return 1;
    }
    exercise_isa_families(initialized_value, (struct AcceptanceResults *)&acceptance_results);
    acceptance_result = 0xA11CE55Au;
    return 0;
}
