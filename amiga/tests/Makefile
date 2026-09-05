CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic
DRIVER_ROOT := ..
LIB_ROOT ?= ../../../fujinet-nio-lib
BUILD_DIR := build
TEST := $(BUILD_DIR)/test_fujinet_disk_driver
LINK_TEST := $(BUILD_DIR)/test_fujinet_nio_client_link
RETRY_TEST := $(BUILD_DIR)/test_fujinet_nio_client_retry
QUEUE_TEST := $(BUILD_DIR)/test_fujinet_io_queue
EXEC_BOUNDARY_TEST := $(BUILD_DIR)/test_fujinet_exec_boundary
RESIDENT_TEST := $(BUILD_DIR)/test_fujinet_disk_resident
NIO_DEVICE_TEST := $(BUILD_DIR)/test_fujinet_nio_device
SERIAL_CHANNEL_TEST := $(BUILD_DIR)/test_fujinet_nio_serial_channel
EXCHANGE_OPTS_TEST := $(BUILD_DIR)/test_fujinet_nio_exchange_opts
LIBRARY := $(LIB_ROOT)/build/fujinet-nio-linux.a

.PHONY: all test library clean

all: $(TEST) $(LINK_TEST) $(RETRY_TEST) $(QUEUE_TEST) $(EXEC_BOUNDARY_TEST) $(RESIDENT_TEST) \
	$(NIO_DEVICE_TEST) $(SERIAL_CHANNEL_TEST) $(EXCHANGE_OPTS_TEST)

$(TEST): test_fujinet_disk_driver.c \
		$(DRIVER_ROOT)/common/fujinet_disk_driver.c \
		$(DRIVER_ROOT)/common/fujinet_disk_dos_envec.c \
		$(DRIVER_ROOT)/common/fujinet_disk_filesystem.c \
		$(DRIVER_ROOT)/common/fujinet_disk_media_profile.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include -I$(LIB_ROOT)/include $^ -o $@

library:
	$(MAKE) -C $(LIB_ROOT) linux

$(LIBRARY): library


$(LINK_TEST): library test_fujinet_nio_client_link.c \
		$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c \
		$(DRIVER_ROOT)/include/fujinet_nio_endian.h $(LIBRARY)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_client_link.c \
		$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c \
		$(LIBRARY) -o $@

$(RETRY_TEST): library test_fujinet_nio_client_retry.c \
		$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c \
		$(DRIVER_ROOT)/include/fujinet_nio_endian.h $(LIBRARY)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_client_retry.c $(LIBRARY) \
		-Wl,--wrap=fn_transport_exchange_buffers -o $@

$(QUEUE_TEST): test_fujinet_io_queue.c $(DRIVER_ROOT)/common/fujinet_io_queue.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include $^ -o $@

$(EXEC_BOUNDARY_TEST): test_fujinet_exec_boundary.c \
		$(DRIVER_ROOT)/common/fujinet_io_queue.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include $^ -o $@

$(RESIDENT_TEST): test_fujinet_disk_resident.c \
		$(DRIVER_ROOT)/disk.device/fujinet_disk_device.c \
		$(DRIVER_ROOT)/common/fujinet_disk_driver.c \
		$(DRIVER_ROOT)/common/fujinet_io_queue.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-const-variable -DFUJINET_DISK_NATIVE_TEST -Istubs -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include -I$(LIB_ROOT)/include $^ -o $@

$(NIO_DEVICE_TEST): test_fujinet_nio_device.c \
		$(DRIVER_ROOT)/nio.device/fujinet_nio_device.c \
		$(DRIVER_ROOT)/common/fujinet_io_queue.c \
		$(DRIVER_ROOT)/include/fujinet_nio_endian.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -DFUJINET_NIO_NATIVE_TEST -Istubs \
		-I$(DRIVER_ROOT)/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_device.c \
		$(DRIVER_ROOT)/nio.device/fujinet_nio_device.c \
		$(DRIVER_ROOT)/common/fujinet_io_queue.c -o $@

$(SERIAL_CHANNEL_TEST): test_fujinet_nio_serial_channel.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(LIB_ROOT)/include $^ -o $@

$(EXCHANGE_OPTS_TEST): test_fujinet_nio_exchange_opts.c \
		$(DRIVER_ROOT)/tools/fujinet_nio_exchange_opts.c \
		$(DRIVER_ROOT)/include/fujinet_nio_endian.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/tools -I$(DRIVER_ROOT)/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_exchange_opts.c \
		$(DRIVER_ROOT)/tools/fujinet_nio_exchange_opts.c -o $@

test: $(TEST) $(LINK_TEST) $(RETRY_TEST) $(QUEUE_TEST) $(EXEC_BOUNDARY_TEST) $(RESIDENT_TEST) \
		$(NIO_DEVICE_TEST) $(SERIAL_CHANNEL_TEST) $(EXCHANGE_OPTS_TEST)
	./$(TEST)
	./$(LINK_TEST)
	./$(RETRY_TEST)
	./$(QUEUE_TEST)
	./$(EXEC_BOUNDARY_TEST)
	./$(RESIDENT_TEST)
	./$(NIO_DEVICE_TEST)
	./$(SERIAL_CHANNEL_TEST)
	./$(EXCHANGE_OPTS_TEST)

clean:
	rm -rf $(BUILD_DIR)
