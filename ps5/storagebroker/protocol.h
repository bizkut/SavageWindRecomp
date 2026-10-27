/* SPDX-License-Identifier: GPL-3.0-or-later
 * Savage Wind storage broker protocol. Behavioral reference: the independent
 * PS5 payload write broker and its ps5_writebroker.h; this is a separate,
 * incompatible protocol and does not share its port, token or authority.
 *
 * Local little-endian ABI, version 1. All integers are fixed-width. A hello
 * precedes requests; requests and replies are synchronous on each connection.
 * Paths are nonempty relative byte strings without NUL, '.' or '..' components.
 * A request consists of its header, path bytes, then (for WRITE/REPLACE)
 * length data bytes. A successful READ reply is followed by result data bytes.
 * Invalid framing closes the connection. Results are byte counts, zero, or
 * negative errno. STAT returns a regular file's size in reply.value.
 */
#ifndef SAVAGE_STORAGE_PROTOCOL_H
#define SAVAGE_STORAGE_PROTOCOL_H
#include <stdint.h>

#ifndef SB_PORT
#define SB_PORT 9142
#endif
#define SB_MAGIC UINT32_C(0x31574253) /* SBW1 */
#define SB_VERSION 2u
#define SB_TOKEN_BYTES 32u
#ifndef SB_ROOT
#define SB_ROOT "/data/savagewind"
#endif
#define SB_TOKEN_NAME ".storage-broker-token"
#define SB_MAX_PATH 1024u
#define SB_MAX_TRANSFER (16u * 1024u * 1024u)

enum sb_operation {
    SB_CREATE = 1, SB_WRITE, SB_READ, SB_STAT, SB_REPLACE, SB_UNLINK,
    SB_MKDIR, SB_QUIT
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
_Static_assert(sizeof(struct sb_hello) == 40, "hello wire size");
_Static_assert(sizeof(struct sb_request) == 24, "request wire size");
_Static_assert(sizeof(struct sb_reply) == 24, "reply wire size");
#endif
