/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "import_status.h"
#include <string.h>
#include <limits.h>

struct mapping {
    const char *key, *text;
};
static const struct mapping stages[] = {{"checking_packages", "Checking packages"},
                                        {"checking_keys", "Checking keys"},
                                        {"awaiting_confirm", "Confirm import"},
                                        {"extracting_exefs", "Extracting ExeFS"},
                                        {"extracting_romfs", "Merging and extracting RomFS"},
                                        {"verifying", "Verifying"},
                                        {"publishing", "Publishing"},
                                        {"ready", "Ready for PC code build"},
                                        {"failed", "Import failed"},
                                        {"cancelled", "Import cancelled"}};
static const struct mapping errors[] = {
    {"base_missing", "Base game missing. Add the Breath of the Wild base NSP or XCI."},
    {"update_missing", "Update missing. Add the 1.9.0 update NSP."},
    {"wrong_title", "Wrong game. Add Breath of the Wild packages."},
    {"wrong_version", "Wrong version. Add the 1.9.0 update."},
    {"ambiguous_content", "Multiple matching contents found. Remove duplicate packages."},
    {"content_missing", "Required content missing. Replace the package."},
    {"content_corrupt", "Content is corrupt or cannot be decrypted. Check the package and keys."},
    {"keys_missing", "Keys missing. Add your prod.keys."},
    {"keys_invalid", "Keys invalid. Supply valid console keys."},
    {"ticket_missing", "Ticket missing. Supply the correct ticket or title keys."},
    {"unsupported_container", "Unsupported container. Use NSP or XCI, not NSZ or XCZ."},
    {"insufficient_space", "Not enough free space. Free space and retry."},
    {"io_error", "Storage error. Check the input and available storage."},
    {"cancelled", "Import cancelled."},
    {"stale_request", "Confirmation timed out. Retry the import."},
    {"busy", "Another import is running. Wait and retry."},
    {"internal", "Import failed internally. Retry."}};
static const char *lookup(const struct mapping *m, size_t n, const char *s)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(m[i].key, s))
            return m[i].text;
    return NULL;
}
const char *import_stage_text(const char *s)
{
    return lookup(stages, sizeof(stages) / sizeof(*stages), s);
}
const char *import_error_text(const char *s)
{
    return lookup(errors, sizeof(errors) / sizeof(*errors), s);
}
int import_percent(const struct import_status *s)
{
    if (!s->bytes_total)
        return -1;
    if (s->bytes_done >= s->bytes_total)
        return 100;
    return (int)((long double)s->bytes_done * 100 / s->bytes_total);
}
static int number(const char *s, uint64_t *v)
{
    uint64_t n = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        unsigned d = (unsigned char)*s - '0';
        if (d > 9 || n > (UINT64_MAX - d) / 10)
            return -1;
        n = n * 10 + d;
    }
    *v = n;
    return 0;
}
static int hexsession(const char *s)
{
    if (strlen(s) != 32)
        return 0;
    for (; *s; s++)
        if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f'))
            return 0;
    return 1;
}
static int valid_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], need;
        uint32_t cp;
        if (c < 0x80) {
            if (c < 32 || c == 127)
                return 0;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            cp = c & 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            cp = c & 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            cp = c & 7;
        } else
            return 0;
        if (n - i < need)
            return 0;
        for (unsigned j = 0; j < need; j++) {
            unsigned d = s[i++];
            if ((d & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (d & 63);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp > 0x10ffff)
            return 0;
    }
    return 1;
}
/* DLC indices are single digits from 1 through 8, separated by commas. */
static int valid_dlc(const char *value, size_t length)
{
    if (!length)
        return 1;
    if (length > IMPORT_DLC_MAX_TEXT || !(length & 1))
        return 0;
    for (size_t i = 0; i < length; i++) {
        if (i & 1) {
            if (value[i] != ',')
                return 0;
        } else if (value[i] < '1' || value[i] > '8') {
            return 0;
        }
    }
    return 1;
}
int import_status_parse(const char *data, size_t len, const char *session, struct import_status *out)
{
    static const char *keys[] = {"version",       "session",      "seq",         "stage",      "bytes_done",
                                 "bytes_total",   "files_done",   "files_total", "current",    "base_source",
                                 "update_source", "game_version", "error",       "error_file", "message", "dlc"};
    struct import_status s = {0};
    unsigned seen = 0;
    size_t pos = 0;
    if (!len || len > IMPORT_STATUS_MAX || data[len - 1] != '\n' || memchr(data, 0, len))
        return -1;
    while (pos < len) {
        size_t start = pos, eq;
        while (pos < len && data[pos] != '\n')
            pos++;
        eq = start;
        while (eq < pos && data[eq] != '=')
            eq++;
        if (eq == start || eq == pos)
            return -1;
        unsigned k;
        for (k = 0; k < sizeof(keys) / sizeof(*keys); k++)
            if (strlen(keys[k]) == eq - start && !memcmp(keys[k], data + start, eq - start))
                break;
        if (k == sizeof(keys) / sizeof(*keys) || (seen & (1u << k)))
            return -1;
        seen |= 1u << k;
        size_t n = pos - eq - 1;
        char value[IMPORT_STATUS_MAX + 1];
        memcpy(value, data + eq + 1, n);
        value[n] = 0;
        if (!valid_utf8((const unsigned char *)value, n))
            return -1;
        char *dst = NULL;
        size_t cap = 0;
        uint64_t *num = NULL;
        switch (k) {
        case 0:
            if (strcmp(value, "1"))
                return -1;
            break;
        case 1:
            dst = s.session;
            cap = sizeof(s.session);
            break;
        case 2:
            num = &s.seq;
            break;
        case 3:
            dst = s.stage;
            cap = sizeof(s.stage);
            break;
        case 4:
            num = &s.bytes_done;
            break;
        case 5:
            num = &s.bytes_total;
            break;
        case 6:
            num = &s.files_done;
            break;
        case 7:
            num = &s.files_total;
            break;
        case 8:
            dst = s.current;
            cap = sizeof(s.current);
            break;
        case 9:
            dst = s.base_source;
            cap = sizeof(s.base_source);
            break;
        case 10:
            dst = s.update_source;
            cap = sizeof(s.update_source);
            break;
        case 11:
            dst = s.game_version;
            cap = sizeof(s.game_version);
            break;
        case 12:
            dst = s.error;
            cap = sizeof(s.error);
            break;
        case 13:
            dst = s.error_file;
            cap = sizeof(s.error_file);
            break;
        case 14:
            dst = s.message;
            cap = sizeof(s.message);
            break;
        case 15:
            if (!valid_dlc(value, n))
                return -1;
            dst = s.dlc;
            cap = sizeof(s.dlc);
            break;
        }
        if (dst) {
            if (n >= cap)
                return -1;
            memcpy(dst, value, n + 1);
        }
        if (num && number(value, num))
            return -1;
        pos++;
    }
    if ((seen & 255) != 255 || !hexsession(s.session) || !import_stage_text(s.stage) ||
        (*s.error && !import_error_text(s.error)))
        return -1;
    if (strcmp(s.session, session))
        return 0;
    *out = s;
    return 1;
}
