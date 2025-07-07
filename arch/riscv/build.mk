# RISC-V Architecture Configuration

ARCH_DIR := $(SRC_DIR)/arch/$(ARCH)
INC_DIRS += -I $(ARCH_DIR)

# core speed
F_CLK := 10000000

# uart baud rate
SERIAL_BAUDRATE := 57600

# timer interrupt frequency (100 -> 100 ints/s -> 10ms tick time. 0 -> timer0 fixed frequency)
F_TICK := 100

DEFINES := -DF_CPU=$(F_CLK) \
           -DUSART_BAUD=$(SERIAL_BAUDRATE) \
           -DF_TIMER=$(F_TICK) \
           -include config.h

# Toolchain override (default to GNU)
CROSS_COMPILE ?= riscv32-unknown-elf-
TOOLCHAIN_TYPE ?= gnu

# Architecture flags
ARCH_FLAGS = -march=rv32imzicsr -mabi=ilp32

# Common compiler flags
CFLAGS += -Wall -Wextra -Wshadow -Wno-unused-parameter -Werror
CFLAGS += -O2 -std=gnu99
CFLAGS += $(ARCH_FLAGS)
CFLAGS += -mstrict-align -ffreestanding -nostdlib -fomit-frame-pointer
CFLAGS += $(INC_DIRS) $(DEFINES) -fdata-sections -ffunction-sections

ifeq ($(CC_IS_CLANG),1)
ifeq ($(TOOLCHAIN_TYPE),llvm)
    CC    = $(CROSS_COMPILE)clang
    AS    = $(CROSS_COMPILE)clang
    LD    = $(CROSS_COMPILE)ld.lld
    DUMP  = $(CROSS_COMPILE)llvm-objdump -M no-aliases
    READ  = $(CROSS_COMPILE)llvm-readelf
    OBJ   = $(CROSS_COMPILE)llvm-objcopy
    SIZE  = $(CROSS_COMPILE)llvm-size
    AR    = $(CROSS_COMPILE)llvm-ar

    CFLAGS += --target=riscv32-unknown-elf
    CFLAGS += -Wno-unused-command-line-argument
    ASFLAGS = --target=riscv32-unknown-elf $(ARCH_FLAGS)
    LDFLAGS = -m elf32lriscv --gc-sections
else
    CC    = $(CC_DEFAULT)
    CC    = $(CROSS_COMPILE)gcc
    AS    = $(CROSS_COMPILE)as
    LD    = $(CROSS_COMPILE)ld
    DUMP  = $(CROSS_COMPILE)objdump -Mno-aliases
    READ  = $(CROSS_COMPILE)readelf
    OBJ   = $(CROSS_COMPILE)objcopy
    SIZE  = $(CROSS_COMPILE)size
    AR    = $(CROSS_COMPILE)ar

    ASFLAGS = $(ARCH_FLAGS)
    LDFLAGS = -melf32lriscv --gc-sections
endif

AR    = $(CROSS_COMPILE)ar

ARFLAGS = r
LDSCRIPT = $(ARCH_DIR)/riscv32-qemu.ld

HAL_OBJS := boot.o hal.o muldiv.o
HAL_OBJS := $(addprefix $(BUILD_KERNEL_DIR)/,$(HAL_OBJS))
deps += $(HAL_OBJS:%.o=%.o.d)

$(BUILD_KERNEL_DIR)/%.o: $(ARCH_DIR)/%.c | $(BUILD_DIR)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ -c -MMD -MF $@.d $<

run:
	@$(call notice, Ready to launch Linmo kernel + application.)
	$(Q)qemu-system-riscv32 -machine virt -nographic -bios none -kernel $(BUILD_DIR)/image.elf -nographic
