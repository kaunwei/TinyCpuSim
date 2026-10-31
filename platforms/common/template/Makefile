CC = arm-none-eabi-gcc
OPT ?= -O0
CFLAGS ?= -mcpu=cortex-m3 -mthumb -ffreestanding -nostdlib $(OPT) -g -Wall -Wextra
LDFLAGS ?= -T linker.ld

SRCS = startup.s main.c
HEADERS = fw_config.h
TARGET = template.elf

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

clean:
	rm -f $(TARGET)
