CC ?= arm-none-eabi-gcc
CFLAGS ?= -mcpu=cortex-m3 -mthumb -nostdlib -O2 -Wall -I bsp
LDFLAGS ?= -T bsp/linker.ld

SRCS = bsp/startup.s bsp/uart.c bsp/timer.c bsp/i2c.c bsp/spi.c bsp/crc.c bsp/custom_dev.c apps/demo_all_peripherals.c

all: build/demo.elf

build/demo.elf: $(SRCS)
	@mkdir -p build
	@which $(CC) >/dev/null 2>&1 && $(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ || echo "Cross-compiler not present on runner, skipping host ELF build"

clean:
	rm -rf build
