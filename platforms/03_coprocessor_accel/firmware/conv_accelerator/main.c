#include <stdint.h>
#include "platform.h"
#include "mailbox.h"

static volatile uint32_t g_accel_done = 0;

void Mailbox_IRQHandler(void) {
    MAILBOX_INTCLR = 1;
    MAILBOX_STATUS = MB_STATUS_RUNNING;

    uint32_t width = MAILBOX_PARAM_WIDTH;
    uint32_t height = MAILBOX_PARAM_HEIGHT;
    uint32_t k_sz = MAILBOX_PARAM_KERNEL_SZ;

    volatile uint8_t* in_ptr = MAILBOX_SCRATCHPAD + CONV_INPUT_OFFSET;
    volatile uint8_t* k_ptr = MAILBOX_SCRATCHPAD + CONV_KERNEL_OFFSET;
    volatile uint8_t* out_ptr = MAILBOX_SCRATCHPAD + CONV_OUTPUT_OFFSET;

    uint32_t out_w = width - k_sz + 1;
    uint32_t out_h = height - k_sz + 1;

    for (uint32_t y = 0; y < out_h; ++y) {
        for (uint32_t x = 0; x < out_w; ++x) {
            uint32_t sum = 0;
            for (uint32_t ky = 0; ky < k_sz; ++ky) {
                for (uint32_t kx = 0; kx < k_sz; ++kx) {
                    sum += in_ptr[(y + ky) * width + (x + kx)] * k_ptr[ky * k_sz + kx];
                }
            }
            out_ptr[y * out_w + x] = (uint8_t)sum;
        }
    }

    MAILBOX_STATUS = MB_STATUS_DONE;
    MAILBOX_DOORBELL_TO_HOST = 1;
    g_accel_done = 1;
}

int main(void) {
    // Enable Mailbox IRQ (IRQ 4) in NVIC
    NVIC->ISER = (1U << 4);

    while (!g_accel_done) {
        __asm__ volatile ("wfi");
    }

    return 0;
}
