TOOLCHAIN ?= $(HOME)/.platformio/packages/toolchain-xtensa-esp-elf/bin
CC         = $(TOOLCHAIN)/xtensa-esp32s3-elf-gcc
STRIP      = $(TOOLCHAIN)/xtensa-esp32s3-elf-strip
OBJDUMP    = $(TOOLCHAIN)/xtensa-esp32s3-elf-objdump

KERNEL_INC = ../src/loader

CFLAGS  = -c -Os -fPIC -fdata-sections -ffunction-sections -Dmain=app_main -I$(KERNEL_INC)
LDFLAGS = -shared -fPIC -nostdlib -nostartfiles -e app_main \
          -Wl,--gc-sections -fvisibility=hidden

STRIP_FLAGS = --strip-unneeded \
              --remove-section=.comment \
              --remove-section=.xt.lit \
              --remove-section=.xt.prop \
              --remove-section=.xtensa.info

MODULES = hello

.PHONY: all clean info

all: $(addsuffix .elf,$(MODULES))

hello.elf: hello/hello.c $(KERNEL_INC)/module_types.h
	$(CC) $(CFLAGS) $< -o hello.o
	$(CC) $(LDFLAGS) hello.o -o $@
	$(STRIP) $(STRIP_FLAGS) $@
	@rm -f hello.o
	@echo "Built $@ ($$(stat -c%s $@) bytes)"

info: hello.elf
	$(OBJDUMP) -h $<
	@echo ""
	$(OBJDUMP) -t $< | grep -E "UND|app_main|module_register|vconsole"

clean:
	rm -f *.elf *.o
