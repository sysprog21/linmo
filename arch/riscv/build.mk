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
           -DF_TIMER=$(F_TICK)

ASFLAGS = -march=rv32imzaicsr -mabi=ilp32
CFLAGS += -Wall -Wextra -Wshadow -Wno-unused-parameter -Werror
CFLAGS += -O2 -std=gnu99
CFLAGS += -march=rv32imazicsr -mabi=ilp32
CFLAGS += -mstrict-align -ffreestanding -nostdlib -fomit-frame-pointer
CFLAGS += $(INC_DIRS) $(DEFINES) -fdata-sections -ffunction-sections
ARFLAGS = r

# Linker flags
LDFLAGS = -melf32lriscv --gc-sections
LDSCRIPT = $(ARCH_DIR)/riscv32-qemu.ld

CROSS_COMPILE ?= riscv-none-elf-
CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)as
LD = $(CROSS_COMPILE)ld
DUMP = $(CROSS_COMPILE)objdump -Mno-aliases
READ = $(CROSS_COMPILE)readelf
OBJ = $(CROSS_COMPILE)objcopy
SIZE = $(CROSS_COMPILE)size
AR = $(CROSS_COMPILE)ar

HAL_OBJS := boot.o hal.o muldiv.o
HAL_OBJS := $(addprefix $(BUILD_KERNEL_DIR)/,$(HAL_OBJS))
deps += $(HAL_OBJS:%.o=%.o.d)

$(BUILD_KERNEL_DIR)/%.o: $(ARCH_DIR)/%.c | $(BUILD_DIR)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ -c -MMD -MF $@.d $<

run:
	@$(call notice, Ready to launch Linmo kernel + application.)
	$(Q)qemu-system-riscv32 -smp 4 -machine virt -nographic -bios none -kernel $(BUILD_DIR)/image.elf -nographic
