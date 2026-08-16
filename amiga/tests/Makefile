CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic
DRIVER_ROOT := ..
LIB_ROOT ?= ../../../fujinet-nio-lib
BUILD_DIR := build
TEST := $(BUILD_DIR)/test_fujinet_disk_driver
LINK_TEST := $(BUILD_DIR)/test_fujinet_nio_client_link
QUEUE_TEST := $(BUILD_DIR)/test_fujinet_io_queue
EXEC_BOUNDARY_TEST := $(BUILD_DIR)/test_fujinet_exec_boundary
RESIDENT_TEST := $(BUILD_DIR)/test_fujinet_disk_resident
LIBRARY := $(LIB_ROOT)/build/fujinet-nio-linux.a

.PHONY: all test library clean

all: $(TEST) $(LINK_TEST) $(QUEUE_TEST) $(EXEC_BOUNDARY_TEST) $(RESIDENT_TEST)

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
		$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c $(LIBRARY)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(DRIVER_ROOT)/public/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_client_link.c \
		$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c \
		$(LIBRARY) -o $@

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

test: $(TEST) $(LINK_TEST) $(QUEUE_TEST) $(EXEC_BOUNDARY_TEST) $(RESIDENT_TEST)
	./$(TEST)
	./$(LINK_TEST)
	./$(QUEUE_TEST)
	./$(EXEC_BOUNDARY_TEST)
	./$(RESIDENT_TEST)

clean:
	rm -rf $(BUILD_DIR)
