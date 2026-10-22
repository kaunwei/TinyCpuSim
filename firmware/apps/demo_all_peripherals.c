#include "../bsp/platform.h"
#include "../bsp/uart.h"
#include "../bsp/timer.h"
#include "../bsp/i2c.h"
#include "../bsp/spi.h"
#include "../bsp/crc.h"
#include "../bsp/custom_dev.h"

volatile uint32_t g_timer_ticks = 0;

void Timer_IRQHandler(void) {
    TIMER0->INTCLR = 1U;
    g_timer_ticks++;
}

int main(void) {
    // Configure Turbo Baudrate / Clock Dividers (1 CPU tick per protocol bit)
    uart_init(1U);
    i2c_init(1U);
    spi_init(1U);
    custom_accel_init(4U);

    uart_puts("START_DEMO\n");

    // 1. Custom Device
    if (custom_accel_add(10, 32) != 42) return 1;

    // 2. CRC32
    if (crc_calc(0x12345678) == 0xFFFFFFFF) return 2;

    // 3. SPI Flash ID
    if (spi_read_jedec_id() != 0xEF) return 3;

    // 4. I2C EEPROM
    i2c_write_byte(0x50, 0x10, 0x5A);

    // 5. Timer & NVIC Interrupt Wait
    NVIC->ISER = (1u << 1);
    timer_init(20U, 1U);
    while (g_timer_ticks == 0U) {}

    uart_puts("ALL_PERIPHERALS_OK\n");
    return 0;
}
