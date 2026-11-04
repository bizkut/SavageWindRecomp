// PS5 control tools' synchronized guest-memory access.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ps5_control_copy(uint64_t addr, void *buffer, size_t size, bool write);
bool ps5_control_region(uint64_t addr, uint64_t *end, bool *readable, bool *writable);
