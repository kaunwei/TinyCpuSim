#pragma once
#include "platform.h"

void dma_init(uint32_t clkdiv);
void dma_memcpy32_poll(uint32_t src_addr, uint32_t dst_addr, uint32_t word_count);
