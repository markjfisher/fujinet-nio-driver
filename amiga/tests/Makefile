CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic
DRIVER_ROOT := ..
LIB_ROOT ?= ../../../fujinet-nio-lib
BUILD_DIR := build
TEST := $(BUILD_DIR)/test_fujinet_disk_driver
LINK_TEST := $(BUILD_DIR)/test_fujinet_nio_client_link
LIBRARY := $(LIB_ROOT)/build/fujinet-nio-linux.a

.PHONY: all test library clean

all: $(TEST) $(LINK_TEST)

$(TEST): test_fujinet_disk_driver.c $(DRIVER_ROOT)/common/fujinet_disk_driver.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(LIB_ROOT)/include $^ -o $@

library:
	$(MAKE) -C $(LIB_ROOT) linux

$(LIBRARY): library


$(LINK_TEST): library test_fujinet_nio_client_link.c \
		$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c $(LIBRARY)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(DRIVER_ROOT)/include -I$(LIB_ROOT)/include \
		test_fujinet_nio_client_link.c \
		$(DRIVER_ROOT)/channels/rs232/fujinet_nio_client.c \
		$(LIBRARY) -o $@

test: $(TEST) $(LINK_TEST)
	./$(TEST)
	./$(LINK_TEST)

clean:
	rm -rf $(BUILD_DIR)
