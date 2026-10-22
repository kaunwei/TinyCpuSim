#include "i2c.h"
void i2c_write_byte(uint8_t slave_addr, uint8_t reg_addr, uint8_t data) {
    I2C0->ADDR = slave_addr;
    I2C0->CR = 1; // START
    I2C0->DR = reg_addr;
    I2C0->DR = data;
    I2C0->CR = 2; // STOP
}
