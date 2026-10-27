CC = arm-none-eabi-gcc
OPT ?= -O0
CFLAGS ?= -mcpu=cortex-m3 -mthumb -ffreestanding -nostdlib $(OPT) -g -Wall -Wextra
LDFLAGS ?= -T linker.ld

SRCS = startup.s main.c
TARGET = template.elf

all: $(TARGET)

$(TARGET): $(SRCS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

clean:
	rm -f $(TARGET)
