#include "spi.h"
void spi_init(uint32_t clkdiv) {
    SPI0->CLKDIV = clkdiv;
}
uint32_t spi_read_jedec_id(void) {
    SPI0->CS = 0U;
    while (!(SPI0->SR & SPI_SR_TXE)) {}
    SPI0->DR = 0x9FU;
    while (SPI0->SR & SPI_SR_BSY) {}
    uint32_t id = SPI0->DR;
    SPI0->CS = 1U;
    return id;
}
