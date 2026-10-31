/* SPDX-License-Identifier: GPL-3.0-or-later
 * Savage Wind storage broker protocol. Behavioral reference: the independent
 * PS5 payload write broker and its ps5_writebroker.h; this is a separate,
 * incompatible protocol and does not share its port, token or authority.
 *
 * Local little-endian ABI, version 9. All integers are fixed-width. A hello
 * precedes requests; requests and replies are synchronous on each connection.
 * Paths are nonempty relative byte strings without NUL, '.' or '..' components,
 * except LIST accepts "." to list the root. WRITE, REPLACE and APPEND carry
 * length data bytes; RENAME carries length bytes of a second relative path.
 * READ and LIST replies carry result bytes. LIST entries consist of an
 * sb_list_entry header followed by name_length raw name bytes, with no padding.
 * TRUNCATE uses offset as the new size. REPAIR has no path or payload; every
 * REPAIR reply, including errors, is followed by struct sb_repair_stats.
 * Invalid framing closes the connection. Results are byte counts, zero, or
 * negative errno. STAT returns size in value.
 * PACKAGE_OPEN selects one read-only regular file below import/ for this
 * connection; PACKAGE_READ has no path and uses that file until disconnect.
 * INSTALL_HOME_ART has no path, uses offset as enum sb_home_art_kind, and
 * carries length bytes. REMOVE_HOME_BACKGROUNDS has no path or payload.
 * GET_VIDEO_MODE has no path or payload and returns the signed 32-bit mode
 * in the low 32 bits of value; failures return negative errno.
 * Version 5 through 8 clients retain their operations.
 */
#ifndef SAVAGE_STORAGE_PROTOCOL_H
#define SAVAGE_STORAGE_PROTOCOL_H
#include <stdint.h>

#ifndef SB_PORT
#define SB_PORT 9142
#endif
#define SB_MAGIC UINT32_C(0x31574253) /* SBW1 */
#define SB_VERSION 10u /* refresh replaced data roots before serving connections */
#define SB_TOKEN_BYTES 32u
#ifndef SB_ROOT
#define SB_ROOT "/data/savagewind"
#endif
#define SB_TOKEN_NAME ".storage-broker-token"
#define SB_MAX_PATH 1024u
#define SB_MAX_TRANSFER (16u * 1024u * 1024u)
#define SB_REPAIR_MAX_ENTRIES 100000u
#define SB_REPAIR_MAX_DEPTH 64u

enum sb_operation {
    SB_CREATE = 1, SB_WRITE, SB_READ, SB_STAT, SB_REPLACE, SB_UNLINK,
    SB_MKDIR, SB_QUIT, SB_APPEND, SB_TRUNCATE, SB_FSYNC, SB_RENAME,
    SB_RMDIR, SB_LIST, SB_REPAIR, SB_CREATE_EXCLUSIVE, SB_PACKAGE_OPEN, SB_PACKAGE_READ,
    SB_INSTALL_HOME_ART, SB_REMOVE_HOME_BACKGROUNDS, SB_GET_VIDEO_MODE
};
enum sb_home_art_kind { SB_HOME_ART_ICON0 = 1, SB_HOME_ART_PIC0, SB_HOME_ART_PIC1 };
#define SB_HOME_ART_DDS_SIZE 8294548u
#define SB_HOME_ART_ICON_MAX (2u * 1024u * 1024u)
enum sb_entry_type { SB_ENTRY_REGULAR = 1, SB_ENTRY_DIRECTORY = 2 };
struct sb_list_entry {
    uint32_t type;
    uint32_t name_length;
};
struct sb_repair_stats {
    uint32_t scanned;
    uint32_t changed;
    uint32_t skipped;
    uint32_t failed;
};
struct sb_hello {
    uint32_t magic;
    uint32_t version;
    unsigned char token[SB_TOKEN_BYTES];
};
struct sb_request {
    uint32_t magic;
    uint32_t operation;
    uint32_t path_length;
    uint32_t length;
    uint64_t offset;
};
struct sb_reply {
    uint32_t magic;
    int32_t result;
    uint32_t version;
    uint32_t reserved;
    uint64_t value;
};
#ifdef __cplusplus
static_assert(sizeof(struct sb_hello) == 40, "hello wire size");
static_assert(sizeof(struct sb_request) == 24, "request wire size");
static_assert(sizeof(struct sb_reply) == 24, "reply wire size");
static_assert(sizeof(struct sb_list_entry) == 8, "list entry wire size");
static_assert(sizeof(struct sb_repair_stats) == 16, "repair stats wire size");
#else
_Static_assert(sizeof(struct sb_hello) == 40, "hello wire size");
_Static_assert(sizeof(struct sb_request) == 24, "request wire size");
_Static_assert(sizeof(struct sb_reply) == 24, "reply wire size");
_Static_assert(sizeof(struct sb_list_entry) == 8, "list entry wire size");
_Static_assert(sizeof(struct sb_repair_stats) == 16, "repair stats wire size");
#endif
#endif
