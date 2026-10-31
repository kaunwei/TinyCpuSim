#pragma once
#include <stdint.h>

#define UART0_BASE       0x40000000U
#define TIMER0_BASE      0x40001000U
#define I2C0_BASE        0x40002000U
#define SPI0_BASE        0x40003000U
#define CRC0_BASE        0x40004000U
#define GPIO0_BASE       0x40005000U
#define DMA0_BASE        0x40006000U
#define CUSTOM0_BASE     0x50000000U
#define NVIC_BASE        0xE000E000U

typedef struct {
    volatile uint32_t DATA;
    volatile uint32_t STATUS;
    volatile uint32_t CONTROL;
    volatile uint32_t BAUD;
    volatile uint32_t CLKDIV;
} UART_TypeDef;

#define UART_STATUS_TX_READY (1U << 0)
#define UART_STATUS_RX_VALID (1U << 1)
#define UART_STATUS_TX_EMPTY (1U << 2)
#define UART_STATUS_BUSY     (1U << 3)
#define UART_STATUS_OVR      (1U << 4)

typedef struct {
    volatile uint32_t LOAD;
    volatile uint32_t VALUE;
    volatile uint32_t CONTROL;
    volatile uint32_t INTCLR;
    volatile uint32_t RIS;
    volatile uint32_t MIS;
    volatile uint32_t PSC;
} TIMER_TypeDef;

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t ADDR;
    volatile uint32_t CLKDIV;
} I2C_TypeDef;

#define I2C_SR_BUSY (1U << 0)
#define I2C_SR_ACK  (1U << 1)
#define I2C_SR_RXNE (1U << 2)
#define I2C_SR_TXE  (1U << 3)
#define I2C_SR_OVR  (1U << 4)

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CS;
    volatile uint32_t CLKDIV;
} SPI_TypeDef;

#define SPI_SR_BSY  (1U << 0)
#define SPI_SR_TXE  (1U << 1)
#define SPI_SR_RXNE (1U << 2)
#define SPI_SR_OVR  (1U << 3)

typedef struct {
    volatile uint32_t DR;
    volatile uint32_t INIT;
    volatile uint32_t CR;
} CRC_TypeDef;

typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t IER;
    volatile uint32_t ISR;
    volatile uint32_t DEBOUNCE;
    volatile uint32_t BUSY;
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t SRC;
    volatile uint32_t DST;
    volatile uint32_t CNDTR;
    volatile uint32_t CR;
    volatile uint32_t SR;
    volatile uint32_t CLKDIV;
} DMA_TypeDef;

#define DMA_CR_EN      (1U << 0)
#define DMA_CR_TCIE    (1U << 1)
#define DMA_CR_SRC_INC (1U << 2)
#define DMA_CR_DST_INC (1U << 3)
#define DMA_CR_SIZE_B  (0U << 4)
#define DMA_CR_SIZE_H  (1U << 4)
#define DMA_CR_SIZE_W  (2U << 4)

#define DMA_SR_BUSY    (1U << 0)
#define DMA_SR_TCIF    (1U << 1)
#define DMA_SR_TEIF    (1U << 2)
#define DMA_SR_OVR     (1U << 3)

typedef struct {
    volatile uint32_t REG_A;
    volatile uint32_t REG_B;
    volatile uint32_t CMD;
    volatile uint32_t RESULT;
    volatile uint32_t STATUS;
    volatile uint32_t LATENCY;
} CUSTOM_TypeDef;

#define CUSTOM_STATUS_DONE (1U << 0)
#define CUSTOM_STATUS_BUSY (1U << 1)
#define CUSTOM_STATUS_OVR  (1U << 2)

typedef struct {
    volatile uint32_t ISER;
    uint32_t RESERVED0[31];
    volatile uint32_t ICER;
    uint32_t RESERVED1[31];
    volatile uint32_t ISPR;
    uint32_t RESERVED2[31];
    volatile uint32_t ICPR;
} NVIC_TypeDef;

#define UART0   ((UART_TypeDef*)UART0_BASE)
#define TIMER0  ((TIMER_TypeDef*)TIMER0_BASE)
#define I2C0    ((I2C_TypeDef*)I2C0_BASE)
#define SPI0    ((SPI_TypeDef*)SPI0_BASE)
#define CRC0    ((CRC_TypeDef*)CRC0_BASE)
#define GPIO0   ((GPIO_TypeDef*)GPIO0_BASE)
#define DMA0    ((DMA_TypeDef*)DMA0_BASE)
#define CUSTOM0 ((CUSTOM_TypeDef*)CUSTOM0_BASE)
#define NVIC    ((NVIC_TypeDef*)(NVIC_BASE + 0x100))
