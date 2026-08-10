LIB_ROOT ?= ../../fujinet-nio-lib
CC := m68k-amigaos-gcc
CFLAGS := -std=c99 -Wall -Wextra -Werror -O2 -mcpu=68000 -msoft-float
CPPFLAGS := -Iinclude -I$(LIB_ROOT)/include
BUILD_DIR := ../build/amiga
LIBRARY := $(LIB_ROOT)/build/fujinet-nio-amiga-driver.a
DEVICE := $(BUILD_DIR)/fujinet-disk.device
SOURCES := common/fujinet_disk_driver.c \
	channels/rs232/fujinet_nio_client.c \
	disk.device/fujinet_disk_device.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all native tests clean

all: tests native

native: $(DEVICE)

$(LIBRARY):
	$(MAKE) -C $(LIB_ROOT) -f makefiles/build.mk \
		TARGET=amiga-driver PROGRAM=fujinet-nio lib

$(DEVICE): $(OBJECTS) $(LIBRARY)
	$(CC) -nostartfiles -mcpu=68000 -msoft-float $^ -lamiga -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

tests:
	$(MAKE) -C tests test LIB_ROOT=$(abspath $(LIB_ROOT))

clean:
	$(MAKE) -C tests clean
	rm -rf $(BUILD_DIR)
