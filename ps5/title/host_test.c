/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "import_status.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *id = "0123456789abcdef0123456789abcdef";
static const char *record = "version=1\nsession=0123456789abcdef0123456789abcdef\nseq=1\nstage=checking_"
                            "packages\nbytes_done=12\nbytes_total=0\nfiles_done=1\nfiles_total=0\n";
int main(void)
{
    struct import_status s;
    char b[5000];
    assert(import_status_parse(record, strlen(record), id, &s) == 1);
    assert(!s.dlc[0]);
    snprintf(b, sizeof(b), "%sdlc=\n", record);
    assert(import_status_parse(b, strlen(b), id, &s) == 1);
    assert(!s.dlc[0]);
    snprintf(b, sizeof(b), "%sdlc=1,2,8\n", record);
    assert(import_status_parse(b, strlen(b), id, &s) == 1);
    assert(!strcmp(s.dlc, "1,2,8"));
    snprintf(b, sizeof(b), "%sdlc=1,2,3,4,5,6,7,8\n", record);
    assert(import_status_parse(b, strlen(b), id, &s) == 1);
    const char *invalid_dlc[] = {"0", "9", "10", "1,9", "1,", ",1", "1,,2", "1, 2",
                                 "1,2,3,4,5,6,7,8,1"};
    for (size_t i = 0; i < sizeof(invalid_dlc) / sizeof(*invalid_dlc); i++) {
        snprintf(b, sizeof(b), "%sdlc=%s\n", record, invalid_dlc[i]);
        assert(import_status_parse(b, strlen(b), id, &s) == -1);
    }
    assert(import_percent(&s) == -1);
    s.bytes_total = 24;
    assert(import_percent(&s) == 50);
    assert(import_status_parse(record, strlen(record), "ffffffffffffffffffffffffffffffff", &s) == 0);
    snprintf(b, sizeof(b), "%sunknown=x\n", record);
    assert(import_status_parse(b, strlen(b), id, &s) == -1);
    memset(b, 'x', sizeof(b));
    b[sizeof(b) - 1] = '\n';
    assert(import_status_parse(b, sizeof(b), id, &s) == -1);
    assert(import_status_parse(record, strlen(record) - 1, id, &s) == -1);
    snprintf(b, sizeof(b), "%sseq=2\n", record);
    assert(import_status_parse(b, strlen(b), id, &s) == -1);
    snprintf(b, sizeof(b),
             "version=1\nsession=%s\nseq=no\nstage=ready\nbytes_done=0\nbytes_total=0\nfiles_done=0\nfiles_"
             "total=0\n",
             id);
    assert(import_status_parse(b, strlen(b), id, &s) == -1);
    snprintf(b, sizeof(b),
             "version=1\nsession=%s\nseq=18446744073709551616\nstage=ready\nbytes_done=0\nbytes_total="
             "0\nfiles_done=0\nfiles_total=0\n",
             id);
    assert(import_status_parse(b, strlen(b), id, &s) == -1);
    snprintf(b, sizeof(b),
             "version=1\nsession=%s\nseq=1\nstage=unknown\nbytes_done=0\nbytes_total=0\nfiles_done=0\nfiles_"
             "total=0\n",
             id);
    assert(import_status_parse(b, strlen(b), id, &s) == -1);
    snprintf(b, sizeof(b), "%smessage=\xc0\x80\n", record);
    assert(import_status_parse(b, strlen(b), id, &s) == -1);
    snprintf(b, sizeof(b), "%smessage=a=b\n", record);
    assert(import_status_parse(b, strlen(b), id, &s) == 1);
    assert(!strcmp(s.message, "a=b"));
    assert(!strcmp(import_stage_text("ready"), "Ready for PC code build"));
    assert(!strcmp(import_stage_text("failed"), "Import failed"));
    assert(!strcmp(import_stage_text("cancelled"), "Import cancelled"));
    const char *codes[] = {
        "base_missing",      "update_missing",  "wrong_title",           "wrong_version",
        "ambiguous_content", "content_missing", "content_corrupt",       "keys_missing",
        "keys_invalid",      "ticket_missing",  "unsupported_container", "insufficient_space",
        "io_error",          "cancelled",       "stale_request",         "busy",
        "internal"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); i++)
        assert(import_error_text(codes[i]));
    assert(!import_error_text("unknown"));
    assert(!import_stage_text("unknown"));
    puts("import status tests passed");
    return 0;
}
