WASM    ?= wasm -q
WCC     ?= wcc -q
WLINK   ?= wlink OPTION quiet

BUILD_DIR ?= build/dos
TARGET    := $(BUILD_DIR)/fujinet.sys
MAP       := $(BUILD_DIR)/fujinet.map

VERSION := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)$(shell git status --porcelain 2>/dev/null | grep -q '^[ MADRCU]' && echo '*')

INCLUDES := -Iinclude -Isrc/driver -Isrc/nio -Isrc/serial -Isrc/util
CFLAGS   := -0 -bt=dos -ms $(INCLUDES) -s -osh -zu -DVERSION=\"$(VERSION)\"
ASFLAGS  := -0 -mt -bt=DOS

C_SRCS := \
	src/driver/commands.c \
	src/nio/nio.c \
	src/nio/nio_disk_protocol.c \
	src/nio/nio_protocol.c \
	src/nio/nio_transaction.c \
	src/driver/dispatch.c \
	src/serial/port_config.c \
	src/serial/id8250.c \
	src/driver/init.c \
	src/util/print.c \
	src/driver/setf5.c \
	src/driver/intf5.c

ASM_SRCS := \
	src/driver/header.asm \
	src/serial/portio.asm \
	src/driver/iwrap.asm

OBJS := \
	$(patsubst %.c,$(BUILD_DIR)/%.obj,$(notdir $(C_SRCS))) \
	$(patsubst %.asm,$(BUILD_DIR)/%.obj,$(notdir $(ASM_SRCS)))

vpath %.c src/driver src/nio src/serial src/util
vpath %.asm src/driver src/serial

.PHONY: all sys tests clean

all: sys

sys: $(TARGET)

tests:
	$(MAKE) -C tests test

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/init.obj: src/driver/init.c | $(BUILD_DIR)
	$(WCC) $(CFLAGS) -nt=_INIT -nc=INIT -fo=$@ $<

$(BUILD_DIR)/%.obj: %.c | $(BUILD_DIR)
	$(WCC) $(CFLAGS) -fo=$@ $<

$(BUILD_DIR)/%.obj: %.asm | $(BUILD_DIR)
	$(WASM) $(ASFLAGS) -fo=$@ $<

$(TARGET): $(OBJS)
	$(WLINK) SYSTEM dos com ORDER clname SYS_HEADER clname DATA clname CODE clname BSS clname INIT OPTION MAP=$(MAP), NODEFAULTLIBS disable 1014, statics name $@ file {$(OBJS)} library {clibs.lib}

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C tests clean
