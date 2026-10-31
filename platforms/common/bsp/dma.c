#include "dma.h"

void dma_init(uint32_t clkdiv) {
    DMA0->CLKDIV = clkdiv;
}

void dma_memcpy32_poll(uint32_t src_addr, uint32_t dst_addr, uint32_t word_count) {
    while (DMA0->SR & DMA_SR_BUSY) {}
    DMA0->SR = DMA_SR_TCIF | DMA_SR_TEIF | DMA_SR_OVR;
    DMA0->SRC = src_addr;
    DMA0->DST = dst_addr;
    DMA0->CNDTR = word_count;
    DMA0->CR = DMA_CR_EN | DMA_CR_SRC_INC | DMA_CR_DST_INC | DMA_CR_SIZE_W;
    while (DMA0->SR & DMA_SR_BUSY) {}
}
