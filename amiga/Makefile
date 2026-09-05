LIB_ROOT ?= ../../fujinet-nio-lib
CC := m68k-amigaos-gcc
CFLAGS := -std=c99 -Wall -Wextra -Werror -O2 -mcpu=68000 -msoft-float
CPPFLAGS := -Iinclude -Ipublic/include -I$(LIB_ROOT)/include
BUILD_DIR := ../build/amiga
LIBRARY := $(LIB_ROOT)/build/fujinet-nio-amiga-driver.a
DEVICE := $(BUILD_DIR)/fujinet-disk.device
DEVICE_MAP := $(DEVICE).map
MOUNT_TOOL := $(BUILD_DIR)/fujinet-mount
RESIDENT_LOADER := $(BUILD_DIR)/fujinet-load-resident
RESIDENT_UNLOADER := $(BUILD_DIR)/fujinet-unload-resident
TD_PROBE := $(BUILD_DIR)/fujinet-td-probe
NIO_DEVICE := $(BUILD_DIR)/fujinet-nio.device
NIO_DEVICE_MAP := $(NIO_DEVICE).map
NIO_EXCHANGE := $(BUILD_DIR)/fujinet-nio-exchange
NIO_BAUD := $(BUILD_DIR)/fujinet-nio-baud
NIO_OBJECTS := $(BUILD_DIR)/nio.device/fujinet_nio_device.o \
	$(BUILD_DIR)/nio.device/fujinet_nio_serial_backend.o \
	$(BUILD_DIR)/common/fujinet_io_queue.o \
	$(BUILD_DIR)/nio-framing/fn_session.o \
	$(BUILD_DIR)/nio-framing/fn_slip.o
SDK_INCLUDE := $(BUILD_DIR)/include
SDK_LIBRARY := $(BUILD_DIR)/lib/libfujinet-amiga-disk.a
SDK_SOURCES := common/fujinet_disk_dos_envec.c common/fujinet_disk_filesystem.c common/fujinet_disk_media_profile.c
SDK_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/sdk/%.o,$(SDK_SOURCES))
SOURCES := common/fujinet_disk_driver.c \
	common/fujinet_disk_dos_envec.c \
	common/fujinet_disk_filesystem.c \
	common/fujinet_disk_media_profile.c \
	common/fujinet_io_queue.c \
	channels/rs232/fujinet_nio_client.c \
	disk.device/fujinet_disk_device.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all native library tests clean

all: tests native sdk

native: library $(DEVICE) $(MOUNT_TOOL) $(RESIDENT_LOADER) $(RESIDENT_UNLOADER) $(TD_PROBE) \
	$(NIO_DEVICE) $(NIO_EXCHANGE) $(NIO_BAUD)

sdk: $(SDK_LIBRARY) $(SDK_INCLUDE)/fujinet-amiga-disk/support.h $(SDK_INCLUDE)/fujinet-amiga-disk/device.h

$(SDK_LIBRARY): $(SDK_OBJECTS)
	@mkdir -p $(dir $@)
	m68k-amigaos-ar rcs $@ $^

$(BUILD_DIR)/sdk/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(SDK_INCLUDE)/fujinet-amiga-disk/%.h: public/include/fujinet-amiga-disk/%.h
	@mkdir -p $(dir $@)
	cp $< $@

library:
	$(MAKE) -C $(LIB_ROOT) -f makefiles/build.mk \
		TARGET=amiga-driver PROGRAM=fujinet-nio lib

$(LIBRARY): library

$(DEVICE): $(OBJECTS) $(LIBRARY)
	$(CC) -nostartfiles -mcpu=68000 -msoft-float $^ -lamiga -Wl,-Map,$(DEVICE_MAP) -o $@

$(NIO_DEVICE): $(NIO_OBJECTS)
	$(CC) -nostartfiles -mcpu=68000 -msoft-float $^ -lamiga -Wl,-Map,$(NIO_DEVICE_MAP) -o $@

$(BUILD_DIR)/nio-framing/fn_session.o: $(LIB_ROOT)/src/common/fn_session.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/nio-framing/fn_slip.o: $(LIB_ROOT)/src/common/fn_slip.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/nio.device/fujinet_nio_device.o: include/fujinet_nio_endian.h
$(BUILD_DIR)/channels/rs232/fujinet_nio_client.o: include/fujinet_nio_checksum.h \
		include/fujinet_nio_endian.h

$(NIO_EXCHANGE): tools/fujinet-nio-exchange.c tools/fujinet_nio_exchange_opts.c \
		tools/fujinet_nio_exchange_opts.h include/fujinet_nio_device.h \
		include/fujinet_nio_checksum.h include/fujinet_nio_endian.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -mcrt=clib2 \
		tools/fujinet-nio-exchange.c tools/fujinet_nio_exchange_opts.c -lamiga -o $@

$(NIO_BAUD): tools/fujinet-nio-baud.c include/fujinet_nio_device.h \
		include/fujinet_nio_endian.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -mcrt=clib2 $< -lamiga -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(MOUNT_TOOL): tools/fujinet-mount.c include/fujinet_disk_device.h $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -mcrt=clib2 $< $(LIBRARY) -lamiga -o $@

$(RESIDENT_LOADER): tools/fujinet-load-resident.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -mcrt=clib2 $< -lamiga -o $@

$(RESIDENT_UNLOADER): tools/fujinet-unload-resident.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -mcrt=clib2 $< -lamiga -o $@

$(TD_PROBE): tools/fujinet-td-probe.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -mcrt=clib2 $< -lamiga -o $@

tests:
	$(MAKE) -C tests test LIB_ROOT=$(abspath $(LIB_ROOT))

clean:
	$(MAKE) -C tests clean
	rm -rf $(BUILD_DIR)
