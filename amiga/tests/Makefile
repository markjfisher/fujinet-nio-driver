CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic
DRIVER_ROOT := ..
LIB_ROOT ?= ../../../fujinet-nio-lib
BUILD_DIR := build
TEST := $(BUILD_DIR)/test_fujinet_disk_driver
RETRY_TEST := $(BUILD_DIR)/test_fujinet_nio_client_retry
QUEUE_TEST := $(BUILD_DIR)/test_fujinet_io_queue
EXEC_BOUNDARY_TEST := $(BUILD_DIR)/test_fujinet_exec_boundary
RESIDENT_TEST := $(BUILD_DIR)/test_fujinet_disk_resident
PACKET_BACKEND_TEST := $(BUILD_DIR)/test_fujinet_nio_packet_backend
NIO_DEVICE_TEST := $(BUILD_DIR)/test_fujinet_nio_device
SERIAL_CHANNEL_TEST := $(BUILD_DIR)/test_fujinet_nio_serial_channel
EXCHANGE_OPTS_TEST := $(BUILD_DIR)/test_fujinet_nio_exchange_opts
PAULA_UART_TEST := $(BUILD_DIR)/test_fujinet_paula_uart
LIFECYCLE_TEST := $(BUILD_DIR)/test_fujinet_serial_lifecycle
EXEC_STUB_HEADERS := $(wildcard stubs/exec/*.h stubs/proto/*.h)
LIBRARY := $(LIB_ROOT)/build/fujinet-nio-linux.a

.PHONY: all test library clean

all: $(TEST) $(RETRY_TEST) $(QUEUE_TEST) $(EXEC_BOUNDARY_TEST) $(RESIDENT_TEST) \
	$(NIO_DEVICE_TEST) $(PACKET_BACKEND_TEST) $(SERIAL_CHANNEL_TEST) $(EXCHANGE_OPTS_TEST) \
	$(PAULA_UART_TEST) $(LIFECYCLE_TEST)

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


$(RETRY_TEST): library test_fujinet_nio_client_retry.c \
		$(DRIVER_ROOT)/../common/fujinet_disk_retry.c \
		$(DRIVER_ROOT)/../common/fujinet_disk_retry.h $(LIBRARY)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_client_retry.c \
		$(DRIVER_ROOT)/../common/fujinet_disk_retry.c $(LIBRARY) -o $@

$(QUEUE_TEST): test_fujinet_io_queue.c $(DRIVER_ROOT)/common/fujinet_io_queue.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include $^ -o $@

$(EXEC_BOUNDARY_TEST): test_fujinet_exec_boundary.c \
		$(DRIVER_ROOT)/common/fujinet_io_queue.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include $^ -o $@

$(RESIDENT_TEST): $(EXEC_STUB_HEADERS) test_fujinet_disk_resident.c \
		$(DRIVER_ROOT)/disk.device/fujinet_disk_device.c \
		$(DRIVER_ROOT)/common/fujinet_disk_driver.c \
		$(DRIVER_ROOT)/common/fujinet_io_queue.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-const-variable -DFUJINET_DISK_NATIVE_TEST -Istubs -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include -I$(LIB_ROOT)/include $(filter %.c,$^) -o $@

$(NIO_DEVICE_TEST): $(EXEC_STUB_HEADERS) test_fujinet_nio_device.c \
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

$(EXCHANGE_OPTS_TEST): library test_fujinet_nio_exchange_opts.c \
		$(DRIVER_ROOT)/tools/fujinet_nio_exchange_opts.c \
		$(DRIVER_ROOT)/include/fujinet_nio_endian.h $(LIBRARY)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/tools -I$(DRIVER_ROOT)/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_exchange_opts.c \
		$(DRIVER_ROOT)/tools/fujinet_nio_exchange_opts.c $(LIBRARY) -o $@

$(PAULA_UART_TEST): test_fujinet_paula_uart.c \
		$(DRIVER_ROOT)/serial.device/fujinet_paula_uart.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include \
		test_fujinet_paula_uart.c \
		$(DRIVER_ROOT)/serial.device/fujinet_paula_uart.c -o $@

$(LIFECYCLE_TEST): test_fujinet_serial_lifecycle.c \
		$(DRIVER_ROOT)/serial.device/fujinet_serial_lifecycle.c \
		$(DRIVER_ROOT)/serial.device/fujinet_paula_uart.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/serial.device \
		test_fujinet_serial_lifecycle.c \
		$(DRIVER_ROOT)/serial.device/fujinet_serial_lifecycle.c \
		$(DRIVER_ROOT)/serial.device/fujinet_paula_uart.c -o $@

test: $(TEST) $(RETRY_TEST) $(QUEUE_TEST) $(EXEC_BOUNDARY_TEST) $(RESIDENT_TEST) \
		$(NIO_DEVICE_TEST) $(PACKET_BACKEND_TEST) $(SERIAL_CHANNEL_TEST) $(EXCHANGE_OPTS_TEST) \
		$(PAULA_UART_TEST) $(LIFECYCLE_TEST)
	./$(TEST)
	./$(RETRY_TEST)
	./$(QUEUE_TEST)
	./$(EXEC_BOUNDARY_TEST)
	./$(RESIDENT_TEST)
	./$(NIO_DEVICE_TEST)
	./$(PACKET_BACKEND_TEST)
	./$(SERIAL_CHANNEL_TEST)
	./$(EXCHANGE_OPTS_TEST)
	./$(PAULA_UART_TEST)
	./$(LIFECYCLE_TEST)

clean:
	rm -rf $(BUILD_DIR)

# Explicit unchanged Amiga/library sources: no Linux archive or wrapped transport.
PACKET_SOURCES := test_fujinet_nio_packet_backend.c \
	$(DRIVER_ROOT)/nio.device/fujinet_nio_packet_backend.c \
	$(DRIVER_ROOT)/nio.device/fujinet_nio_device.c \
	$(DRIVER_ROOT)/common/fujinet_io_queue.c \
	$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c \
	$(DRIVER_ROOT)/../common/fujinet_disk_retry.c \
	$(LIB_ROOT)/src/platform/amiga/fn_transport.c \
	$(addprefix $(LIB_ROOT)/src/common/,fn_disk.c fn_raw.c fn_init.c fn_state.c \
	fn_packet_header.c fn_packet_parse_common.c fn_packet_checksum.c \
	fn_packet_checksum_packet.c fn_checksum_fold.c)
PACKET_HEADERS := $(wildcard stubs/exec/*.h stubs/proto/*.h stubs/clib/*.h stubs/dos/*.h) \
	$(wildcard $(DRIVER_ROOT)/include/*.h $(DRIVER_ROOT)/nio.device/*.h) \
	$(wildcard $(DRIVER_ROOT)/public/include/fujinet-amiga-disk/*.h) \
	$(wildcard $(DRIVER_ROOT)/../common/*.h $(LIB_ROOT)/include/*.h)

$(PACKET_BACKEND_TEST): $(PACKET_SOURCES) $(PACKET_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -DFUJINET_NIO_NATIVE_TEST \
		-DFN_AMIGA_EXPLICIT_LIFECYCLE -D__AMIGA__ -I$(DRIVER_ROOT)/include \
		-I$(DRIVER_ROOT)/public/include -I$(DRIVER_ROOT)/nio.device \
		-I$(LIB_ROOT)/include -Istubs $(PACKET_SOURCES) -o $@
