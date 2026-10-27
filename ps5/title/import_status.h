/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SAVAGEWIND_IMPORT_STATUS_H
#define SAVAGEWIND_IMPORT_STATUS_H
#include <stddef.h>
#include <stdint.h>
#define IMPORT_STATUS_MAX 4096
#define IMPORT_DLC_MAX_TEXT 15
struct import_status {
    char session[33], stage[32], current[1024], base_source[512], update_source[512];
    char game_version[64], error[64], error_file[512], message[1024];
    char dlc[IMPORT_DLC_MAX_TEXT + 1];
    uint64_t seq, bytes_done, bytes_total, files_done, files_total;
};
/* Returns 1 for valid matching record, 0 for another session, -1 for invalid. */
int import_status_parse(const char *data, size_t length, const char *session, struct import_status *out);
const char *import_stage_text(const char *stage);
const char *import_error_text(const char *error);
int import_percent(const struct import_status *status);
#endif
