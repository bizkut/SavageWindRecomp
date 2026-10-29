// Package-backed ExeFS streams; no decrypted files are written to disk.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
// Optional process-owned diagnostics, installed before package work starts.
void savagewind_package_set_diagnostic(void (*callback)(const char *stage));
void savagewind_package_set_fast_exefs(int enabled);
void savagewind_package_set_startup_mark(void (*callback)(const char *step));
void *savagewind_exefs_open(const char *keys, const char *const *packages, size_t count, char *why, size_t why_size);
void *savagewind_exefs_file_open(void *handle, const char *name);
int64_t savagewind_exefs_file_size(void *file);
ssize_t savagewind_exefs_file_pread(void *file, void *dst, size_t size, uint64_t offset);
void savagewind_exefs_file_close(void *file);
void savagewind_exefs_close(void *handle);
#ifdef __cplusplus
}
#endif
