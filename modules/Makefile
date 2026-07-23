TOOLCHAIN ?= $(HOME)/.platformio/packages/toolchain-xtensa-esp-elf/bin
CC         = $(TOOLCHAIN)/xtensa-esp32s3-elf-gcc
STRIP      = $(TOOLCHAIN)/xtensa-esp32s3-elf-strip
OBJDUMP    = $(TOOLCHAIN)/xtensa-esp32s3-elf-objdump

KERNEL_INC = ../src/loader
SERVICES_INC = ../src/services

CFLAGS  = -c -Os -fPIC -fdata-sections -ffunction-sections -Dmain=app_main -I$(KERNEL_INC) -I$(SERVICES_INC)
LDFLAGS = -shared -fPIC -nostdlib -nostartfiles -e app_main \
          -Wl,--gc-sections -fvisibility=hidden

STRIP_FLAGS = --strip-unneeded \
              --remove-section=.comment \
              --remove-section=.xt.lit \
              --remove-section=.xt.prop \
              --remove-section=.xtensa.info

MODULES = hello badabi oled eink cardkb demo launcher edit gfxdemo desktop menu rtc date settings nerv home files

.PHONY: all clean

all: $(addsuffix .elf,$(MODULES))

define build_module
$(1).elf: $(1)/$(1).c $$(KERNEL_INC)/module_types.h
	$$(CC) $$(CFLAGS) $$< -o $(1).o
	$$(CC) $$(LDFLAGS) $(1).o -o $$@
	$$(STRIP) $$(STRIP_FLAGS) $$@
	@rm -f $(1).o
	@cp $(1)/$(1).ini $(1).ini 2>/dev/null || true
	@echo "Built $$@ ($$$$(stat -c%s $$@) bytes)"
endef

$(foreach m,$(MODULES),$(eval $(call build_module,$(m))))

clean:
	rm -f *.elf *.o *.ini
