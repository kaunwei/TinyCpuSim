#include <stdint.h>
#include "platform.h"
#include "mailbox.h"

static volatile uint32_t g_conv_done = 0;
static volatile uint32_t* const g_test_result = (volatile uint32_t*)0x0002FFFC;

void Mailbox_IRQHandler(void) {
    MAILBOX_INTCLR = 1;
    g_conv_done = 1;
}

int main(void) {
    // 1. Enable Mailbox IRQ (IRQ 16) in NVIC
    NVIC->ISER = (1U << 16);

    // 2. Initialize 8x8 input image (constant 2) and 3x3 kernel (constant 1)
    volatile uint8_t* in_ptr = MAILBOX_SCRATCHPAD + CONV_INPUT_OFFSET;
    volatile uint8_t* k_ptr = MAILBOX_SCRATCHPAD + CONV_KERNEL_OFFSET;
    volatile uint8_t* out_ptr = MAILBOX_SCRATCHPAD + CONV_OUTPUT_OFFSET;

    for (int i = 0; i < 64; ++i) {
        in_ptr[i] = 2;
    }
    for (int i = 0; i < 9; ++i) {
        k_ptr[i] = 1;
    }
    for (int i = 0; i < 36; ++i) {
        out_ptr[i] = 0;
    }

    // 3. Set parameters: 8x8, kernel size 3
    MAILBOX_PARAM_WIDTH = 8;
    MAILBOX_PARAM_HEIGHT = 8;
    MAILBOX_PARAM_CHANNELS = 1;
    MAILBOX_PARAM_KERNEL_SZ = 3;

    // 4. Ring doorbell to accelerator
    MAILBOX_DOORBELL_TO_ACCEL = 1;

    // 5. Wait for completion interrupt
    while (!g_conv_done) {
        __asm__ volatile ("wfi");
    }

    // 6. Verify 6x6 output: each element should be 2 * 1 * 9 = 18
    int pass = 1;
    for (int i = 0; i < 36; ++i) {
        if (out_ptr[i] != 18) {
            pass = 0;
            break;
        }
    }

    if (pass) {
        *g_test_result = 0xCAFECAFE;
    } else {
        *g_test_result = 0xDEADDEAD;
    }

    return pass ? 0 : 1;
}
