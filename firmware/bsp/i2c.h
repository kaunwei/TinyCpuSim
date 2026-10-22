#pragma once
#include "platform.h"
void i2c_init(uint32_t clkdiv);
void i2c_write_byte(uint8_t slave_addr, uint8_t reg_addr, uint8_t data);
