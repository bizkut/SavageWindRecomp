# SPDX-License-Identifier: GPL-3.0-or-later
# Use the same pinned SDK as the title, regardless of the caller's environment.
override PS5_PAYLOAD_SDK := $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../../.deps/ps5/ps5-payload-sdk)
export PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS += -Wall -Wextra -O2 -pthread
OUT ?= ../../build/ps5/storagebroker
ELF := $(OUT)/storagebroker.elf

.PHONY: all clean host-test
all: $(ELF)

$(ELF): broker.c client.c client.h protocol.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ broker.c client.c -lSceRegMgr

clean:
	rm -rf $(OUT)

HOST_ROOT := /tmp/savagewind-storagebroker-test
HOST_FLAGS := -D_GNU_SOURCE -DSB_HOST_TEST -DSB_ROOT='"$(HOST_ROOT)"' -DSB_PORT=19142 -Wall -Wextra -Werror -O2 -pthread
host-test:
	rm -rf $(HOST_ROOT)
	mkdir -p $(HOST_ROOT)
	$${CC:-cc} $(HOST_FLAGS) -o /tmp/savagewind-storagebroker broker.c client.c
	$${CC:-cc} $(HOST_FLAGS) -o /tmp/savagewind-storagebroker-check host_test.c client.c
	/tmp/savagewind-storagebroker > /tmp/savagewind-storagebroker.log 2>&1 & pid=$$!; \
		trap 'kill $$pid 2>/dev/null || true; rm -rf $(HOST_ROOT)' EXIT; \
		for n in 1 2 3 4 5; do test -f $(HOST_ROOT)/.storage-broker-token && break; sleep .1; done; \
		/tmp/savagewind-storagebroker-check
