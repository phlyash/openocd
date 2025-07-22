BIN2C = ../../../../../src/helper/bin2char.sh

CROSS_COMPILE ?= riscv64-unknown-elf-

CC=$(CROSS_COMPILE)gcc
OBJCOPY=$(CROSS_COMPILE)objcopy
OBJDUMP=$(CROSS_COMPILE)objdump

CFLAGS = -march=rv32imc -mabi=ilp32 -static -nostartfiles -nostdlib -Os -g -fPIC

all: k1921vg015.inc

.PHONY: clean

%.elf: %.c
	$(CC) $(CFLAGS) $< -o $@

%.S: %.c
	$(CC) $(CFLAGS) -S $< -o $@

%.lst: %.elf
	$(OBJDUMP) -S $< > $@

%.bin: %.elf
	$(OBJCOPY) -Obinary $< $@

%.inc: %.bin
	$(BIN2C) < $< > $@

clean:
	-rm -f *.elf *.lst *.bin *.inc
