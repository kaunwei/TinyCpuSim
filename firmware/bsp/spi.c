#include "spi.h"
uint32_t spi_read_jedec_id(void) {
    SPI0->CS = 0;
    SPI0->DR = 0x9F;
    uint32_t id = SPI0->DR;
    SPI0->CS = 1;
    return id;
}
