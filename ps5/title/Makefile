# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
.PHONY: host-test clean
host-test: host_test
	./host_test
host_test: host_test.c import_status.c import_status.h
	$(CC) $(CFLAGS) host_test.c import_status.c -o $@
clean:
	rm -f host_test
