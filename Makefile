ARCH := riscv

# in-tree build
SRC_DIR := .

# Build directories
BUILD_DIR := $(SRC_DIR)/build
BUILD_APP_DIR := $(BUILD_DIR)/app
BUILD_KERNEL_DIR := $(BUILD_DIR)/kernel
BUILD_LIB_DIR := $(BUILD_DIR)/lib

include mk/common.mk
# architecture-specific settings
include arch/$(ARCH)/build.mk

# Include directories
INC_DIRS += -I $(SRC_DIR)/include \
            -I $(SRC_DIR)/include/lib

KERNEL_OBJS := timer.o mqueue.o pipe.o semaphore.o mutex.o logger.o error.o syscall.o task.o main.o
KERNEL_OBJS := $(addprefix $(BUILD_KERNEL_DIR)/,$(KERNEL_OBJS))
deps += $(KERNEL_OBJS:%.o=%.o.d)

LIB_OBJS := ctype.o malloc.o math.o memory.o random.o stdio.o string.o queue.o
LIB_OBJS := $(addprefix $(BUILD_LIB_DIR)/,$(LIB_OBJS))
deps += $(LIB_OBJS:%.o=%.o.d)

# Applications
APPS := coop echo hello mqueues semaphore mutex cond \
        pipes pipes_small pipes_struct prodcons progress \
        rtsched suspend test64 timer timer_kill \
        cpubench test_utils umode

# Output files for __link target
IMAGE_BASE := $(BUILD_DIR)/image
IMAGE_FILES := $(IMAGE_BASE).elf $(IMAGE_BASE).map $(IMAGE_BASE).lst \
               $(IMAGE_BASE).sec $(IMAGE_BASE).cnt $(IMAGE_BASE).bin \
               $(IMAGE_BASE).hex $(BUILD_DIR)/code.txt

# Default target
.DEFAULT_GOAL := linmo

# Phony targets
.PHONY: linmo rebuild clean distclean

# Create build directories
$(BUILD_DIR):
	$(Q)mkdir -p $(BUILD_APP_DIR) $(BUILD_KERNEL_DIR) $(BUILD_LIB_DIR)

# Pattern rules for object files
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) -c -MMD -MF $@.d -o $@ $<

# Main library target
linmo: $(BUILD_DIR)/liblinmo.a

$(BUILD_DIR)/liblinmo.a: $(HAL_OBJS) $(KERNEL_OBJS) $(LIB_OBJS)
	$(VECHO) "  AR\t$@\n"
	$(Q)$(AR) $(ARFLAGS) $@ $^

# Application pattern rule
$(APPS): %: rebuild $(BUILD_APP_DIR)/%.o linmo
	$(Q)$(MAKE) --no-print-directory __link

# Link target - creates all output files
__link: $(IMAGE_FILES)

$(IMAGE_BASE).elf: $(BUILD_APP_DIR)/*.o $(BUILD_DIR)/liblinmo.a $(ENTRY_OBJ)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(LD) $(LDFLAGS) -T$(LDSCRIPT) -Map $(IMAGE_BASE).map -o $@ $(BUILD_APP_DIR)/*.o $(ENTRY_OBJ) -L$(BUILD_DIR) -llinmo

$(IMAGE_BASE).lst: $(IMAGE_BASE).elf
	$(VECHO) "  DUMP\t$@\n"
	$(Q)$(DUMP) --disassemble --reloc $< > $@

$(IMAGE_BASE).sec: $(IMAGE_BASE).elf
	$(VECHO) "  DUMP\t$@\n"
	$(Q)$(DUMP) -h $< > $@

$(IMAGE_BASE).cnt: $(IMAGE_BASE).elf
	$(VECHO) "  DUMP\t$@\n"
	$(Q)$(DUMP) -s $< > $@

$(IMAGE_BASE).bin: $(IMAGE_BASE).elf
	$(VECHO) "  COPY\t$@\n"
	$(Q)$(OBJ) -O binary $< $@

$(IMAGE_BASE).hex: $(IMAGE_BASE).elf
	$(VECHO) "  COPY\t$@\n"
	$(Q)$(OBJ) -R .eeprom -O ihex $< $@

$(BUILD_DIR)/code.txt: $(IMAGE_BASE).bin
	$(VECHO) "  DUMP\t$@\n"
	$(Q)hexdump -v -e '4/1 "%02x" "\n"' $< > $@
	@$(SIZE) $(IMAGE_BASE).elf

# Utility targets
rebuild:
	$(Q)find '$(BUILD_APP_DIR)' -type f -name '*.o' -delete 2>/dev/null || true
	$(Q)mkdir -p $(BUILD_APP_DIR)

clean:
	$(VECHO) "Cleaning build artifacts...\n"
	$(Q)find '$(BUILD_APP_DIR)' '$(BUILD_KERNEL_DIR)' -type f -name '*.o' -delete 2>/dev/null || true
	$(Q)find '$(BUILD_DIR)' -type f \( -name '*.o' -o -name '*~' -o -name 'image.*' -o -name 'code.*' \) -delete 2>/dev/null || true
	$(Q)find '$(SRC_DIR)' -maxdepth 1 -type f -name '*.o' -delete 2>/dev/null || true
	@$(RM) $(deps)

distclean: clean
	$(VECHO) "Deep cleaning...\n"
	$(Q)find '$(BUILD_DIR)' -type f -name '*.a' -delete 2>/dev/null || true
	$(Q)rm -rf '$(BUILD_DIR)' 2>/dev/null || true

-include $(deps)
