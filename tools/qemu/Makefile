PLUGIN := debug_trace_profiler.so
SRC := debug_trace_profiler.c

QEMU_PLUGIN_INC ?=
CC ?= cc
PKG_CONFIG ?= pkg-config

GLIB_CFLAGS := $(shell $(PKG_CONFIG) --cflags glib-2.0 2>/dev/null)

CFLAGS ?= -O2
CFLAGS += -fPIC -shared -fvisibility=hidden
ifneq ($(strip $(QEMU_PLUGIN_INC)),)
    CFLAGS += -I$(QEMU_PLUGIN_INC)
endif
CFLAGS += $(GLIB_CFLAGS)

.PHONY: all clean

all: $(PLUGIN)

$(PLUGIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PLUGIN)
