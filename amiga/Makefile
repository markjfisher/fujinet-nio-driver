LIB_ROOT ?= ../../fujinet-nio-lib
CC := m68k-amigaos-gcc
CFLAGS := -std=c99 -Wall -Wextra -Werror -O2 -mcpu=68000 -msoft-float
CPPFLAGS := -Iinclude -I$(LIB_ROOT)/include
BUILD_DIR := ../build/amiga
LIBRARY := $(LIB_ROOT)/build/fujinet-nio-amiga-driver.a
DEVICE := $(BUILD_DIR)/fujinet-disk.device
MOUNT_TOOL := $(BUILD_DIR)/fujinet-mount
SOURCES := common/fujinet_disk_driver.c \
	common/fujinet_io_queue.c \
	channels/rs232/fujinet_nio_client.c \
	disk.device/fujinet_disk_device.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all native library tests clean

all: tests native

native: library $(DEVICE) $(MOUNT_TOOL)

library:
	$(MAKE) -C $(LIB_ROOT) -f makefiles/build.mk \
		TARGET=amiga-driver PROGRAM=fujinet-nio lib

$(LIBRARY): library

$(DEVICE): $(OBJECTS) $(LIBRARY)
	$(CC) -nostartfiles -mcpu=68000 -msoft-float $^ -lamiga -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(MOUNT_TOOL): tools/fujinet-mount.c include/fujinet_disk_device.h $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -mcrt=clib2 $< $(LIBRARY) -lamiga -o $@

tests:
	$(MAKE) -C tests test LIB_ROOT=$(abspath $(LIB_ROOT))

clean:
	$(MAKE) -C tests clean
	rm -rf $(BUILD_DIR)
