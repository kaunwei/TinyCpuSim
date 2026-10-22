#pragma once
#include "platform.h"
void spi_init(uint32_t clkdiv);
uint32_t spi_read_jedec_id(void);
