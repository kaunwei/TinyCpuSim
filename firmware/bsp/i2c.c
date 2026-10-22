#include "i2c.h"
static void i2c_wait_idle(void) {
    while (I2C0->SR & I2C_SR_BUSY) {}
}
void i2c_init(uint32_t clkdiv) {
    I2C0->CLKDIV = clkdiv;
}
void i2c_write_byte(uint8_t slave_addr, uint8_t reg_addr, uint8_t data) {
    I2C0->ADDR = slave_addr;
    I2C0->CR = 1U; // START
    i2c_wait_idle();
    I2C0->DR = reg_addr;
    i2c_wait_idle();
    I2C0->DR = data;
    i2c_wait_idle();
    I2C0->CR = 2U; // STOP
    i2c_wait_idle();
}
