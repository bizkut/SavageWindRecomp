/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "display.h"
#include "bitmap-font.h"
#include "import_status.h"
#include "platform.h"
#include <ps5platform/klog.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#define ROOT "/data/savagewind/import/"
#define CROSS 0x4000u
#define CIRCLE 0x2000u
#define ELFLDR_PORT 9021
#define SEND_TIMEOUT_MS 10000
#define ELF_CHUNK_BYTES 65536u
#define LOADER_LOG_BYTES 512
#define STATUS_TIMEOUT_MS 10000
#define STATUS_POLL_US 250000
#define PAD_BATCH_SIZE 16
#define SEND_LOADER_UNAVAILABLE (-2)
extern const unsigned char import_elf_start[], import_elf_end[];

enum screen { SCAN, OFFER, WORK, ERROR, DONE, CANCELLED };
enum action { NONE, START, CONFIRM, CANCEL, RESCAN };
/* The render thread only snapshots these fields; filesystem and socket I/O
 * belong to the worker so slow storage cannot block controller input. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static enum screen screen = SCAN;
static enum action action;
static struct import_status status;
static char session[33], failure[1024];
static int package_count;
static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void log_stage(const char *stage)
{
    char b[256];
    snprintf(b, sizeof(b), "savagewind-import-ui: session=%s stage=%s\n", session, stage);
    sceKernelDebugOutText(0, b);
}
static void set_error(const char *s)
{
    pthread_mutex_lock(&lock);
    snprintf(failure, sizeof(failure), "%s", s);
    screen = ERROR;
    pthread_mutex_unlock(&lock);
    char b[1200];
    snprintf(b, sizeof(b), "savagewind-import-ui: session=%s stage=ui_error message=%s\n", session, s);
    sceKernelDebugOutText(0, b);
}
static int atomic_control(const char *name, const char *text)
{
    char tmp[256], path[256];
    snprintf(tmp, sizeof(tmp), ROOT ".%s.tmp", name);
    snprintf(path, sizeof(path), ROOT "%s", name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    size_t n = strlen(text), at = 0;
    while (at < n) {
        ssize_t r = write(fd, text + at, n - at);
        if (r <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        at += (size_t)r;
    }
    if (close(fd) || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
static int create_session(void)
{
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t n = 0;
    while (n < sizeof(b)) {
        ssize_t r = read(fd, b + n, sizeof(b) - n);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        n += (size_t)r;
    }
    close(fd);
    for (unsigned i = 0; i < 16; i++)
        snprintf(session + 2 * i, 3, "%02x", b[i]);
    return 0;
}
/* Keep the loader's output socket drained for the lifetime of the payload, so the payload never blocks on a full
 * socket. Only the first LOADER_LOG_BYTES are copied to klog: that covers elfldr spawn errors and the importer's
 * own early messages (which never contain key material); the rest is counted and discarded. */
static void *drain_loader(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char b[4096], logged[LOADER_LOG_BYTES + 1], line[LOADER_LOG_BYTES + 96];
    size_t kept = 0;
    uint64_t total = 0;
    for (;;) {
        ssize_t r = recv(fd, b, sizeof(b), 0);
        if (r > 0) {
            size_t room = LOADER_LOG_BYTES - kept;
            size_t take = (size_t)r < room ? (size_t)r : room;
            memcpy(logged + kept, b, take);
            kept += take;
            total += (uint64_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    for (size_t i = 0; i < kept; i++)
        if ((unsigned char)logged[i] < 32 || (unsigned char)logged[i] > 126)
            logged[i] = ' ';
    logged[kept] = 0;
    snprintf(line, sizeof(line), "savagewind-import-ui: loader output %llu bytes: %s\n", (unsigned long long)total,
             logged);
    sceKernelDebugOutText(0, line);
    close(fd);
    return NULL;
}
static void log_send_failure(const char *step, int error)
{
    char line[160];
    snprintf(line, sizeof(line), "savagewind-import-ui: session=%s send failed: %s errno=%d\n", session, step, error);
    sceKernelDebugOutText(0, line);
}
/* Stream the embedded ELF to elfldr as ps5-wow's title does (title/main.c wine_launch): a title cannot make the
 * socket non-blocking with fcntl, so every send/recv uses MSG_DONTWAIT under poll(); loader output is read while
 * sending, and the write side is shut only after the last byte. Runs on the worker thread, never the render loop. */
static int send_payload(void)
{
    const char *step = "socket";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_send_failure(step, errno);
        return -1;
    }
    struct sockaddr_in a = {0};
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = htons(ELFLDR_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    step = "connect";
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        int e = errno;
        log_send_failure(step, e);
        close(fd);
        return e == ECONNREFUSED ? SEND_LOADER_UNAVAILABLE : -1;
    }
    const unsigned char *p = import_elf_start;
    size_t left = (size_t)(import_elf_end - import_elf_start);
    uint64_t deadline = now_ms() + SEND_TIMEOUT_MS;
    while (left) {
        struct pollfd d = {.fd = fd, .events = POLLIN | POLLOUT};
        step = "poll";
        int r = poll(&d, 1, 100);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            goto failed;
        }
        if (d.revents & POLLNVAL) {
            errno = EBADF;
            goto failed;
        }
        if (d.revents & (POLLIN | POLLHUP | POLLERR)) {
            /* The loader answering or closing before it has the whole ELF means it rejected the upload. */
            char b[4096];
            step = "loader closed during upload";
            ssize_t n = recv(fd, b, sizeof(b), MSG_DONTWAIT);
            if (n == 0) {
                errno = ECONNRESET;
                goto failed;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                goto failed;
        }
        if (d.revents & POLLOUT) {
            step = "send";
            size_t chunk = left < ELF_CHUNK_BYTES ? left : ELF_CHUNK_BYTES;
            ssize_t n = send(fd, p, chunk, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                p += n;
                left -= (size_t)n;
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                if (n == 0)
                    errno = EPIPE;
                goto failed;
            }
        }
        if (left && now_ms() >= deadline) {
            step = "send timeout";
            errno = ETIMEDOUT;
            goto failed;
        }
    }
    step = "shutdown";
    if (shutdown(fd, SHUT_WR))
        goto failed;
    pthread_t thread;
    step = "drain thread";
    if (pthread_create(&thread, NULL, drain_loader, (void *)(intptr_t)fd))
        goto failed;
    pthread_detach(thread);
    char line[128];
    snprintf(line, sizeof(line), "savagewind-import-ui: session=%s importer sent (%zu bytes)\n", session,
             (size_t)(import_elf_end - import_elf_start));
    sceKernelDebugOutText(0, line);
    return 0;
failed:
    log_send_failure(step, errno);
    close(fd);
    return -1;
}
static int scan(void)
{
    DIR *d = opendir(ROOT);
    if (!d)
        return -1;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *p = strrchr(e->d_name, '.');
        if (p && (!strcasecmp(p, ".nsp") || !strcasecmp(p, ".xci") || !strcasecmp(p, ".nsz") ||
                  !strcasecmp(p, ".xcz")))
            count++;
    }
    closedir(d);
    return count;
}
static void *worker(void *unused)
{
    (void)unused;
    uint64_t launched = 0, last_seq = 0;
    int received = 0, active = 0;
    char prev[32] = "";
    for (;;) {
        pthread_mutex_lock(&lock);
        enum action a = action;
        action = NONE;
        pthread_mutex_unlock(&lock);
        if (a == RESCAN) {
            int n = scan();
            pthread_mutex_lock(&lock);
            package_count = n;
            screen = OFFER;
            pthread_mutex_unlock(&lock);
        }
        if (a == START) {
            if (create_session()) {
                set_error("Cannot generate a secure import session.");
                continue;
            }
            char req[96];
            snprintf(req, sizeof(req), "version=1\nsession=%s\n", session);
            if (atomic_control("request", req)) {
                set_error("Cannot write import request. Check /data/savagewind/import access.");
                continue;
            }
            log_stage("launching");
            int r = send_payload();
            if (r) {
                set_error(r == SEND_LOADER_UNAVAILABLE
                              ? "Import service unavailable. Load the console payload loader and retry."
                              : "Could not send the importer to the console payload loader.");
                continue;
            }
            launched = now_ms();
            received = 0;
            last_seq = 0;
            active = 1;
            prev[0] = 0;
        }
        if ((a == CONFIRM || a == CANCEL) && active) {
            char b[64];
            snprintf(b, sizeof(b), "session=%s\n", session);
            if (atomic_control(a == CONFIRM ? "confirm" : "cancel", b))
                set_error("Cannot write import control file. Check storage access.");
        }
        if (active) {
            int fd = open(ROOT "status", O_RDONLY);
            if (fd >= 0) {
                char b[IMPORT_STATUS_MAX + 1];
                size_t n = 0;
                ssize_t r;
                while (n < sizeof(b) && (r = read(fd, b + n, sizeof(b) - n)) > 0)
                    n += (size_t)r;
                close(fd);
                /* The payload atomically replaces status; reject stale sessions
                 * and older sequence numbers before exposing a snapshot. */
                struct import_status s;
                if (import_status_parse(b, n, session, &s) == 1 && (!received || s.seq > last_seq)) {
                    received = 1;
                    last_seq = s.seq;
                    if (strcmp(prev, s.stage)) {
                        snprintf(prev, sizeof(prev), "%s", s.stage);
                        log_stage(s.stage);
                    }
                    pthread_mutex_lock(&lock);
                    status = s;
                    if (!strcmp(s.stage, "ready")) {
                        screen = DONE;
                        active = 0;
                    } else if (!strcmp(s.stage, "cancelled")) {
                        screen = CANCELLED;
                        active = 0;
                    } else if (!strcmp(s.stage, "failed")) {
                        screen = ERROR;
                        failure[0] = 0;
                        active = 0;
                    }
                    pthread_mutex_unlock(&lock);
                }
            }
            if (!received && now_ms() - launched >= STATUS_TIMEOUT_MS) {
                active = 0;
                set_error("Importer did not publish status within 10 seconds. Retry.");
            }
        }
        usleep(STATUS_POLL_US);
    }
    return NULL;
}
static void rect(int x, int y, int w, int h, uint32_t color)
{
    uint32_t *p = display_image();
    if (x < 0 || y < 0 || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT)
        return;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            p[(size_t)j * DISPLAY_WIDTH + i] = color;
}
static void text(int x, int y, const char *s, int scale, uint32_t color)
{
    int start = x;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c == '\n' || x + 6 * scale > 1800) {
            y += 10 * scale;
            x = start;
            if (c == '\n')
                continue;
        }
        if (y + 7 * scale >= 1040)
            break;
        if (c < 32 || c > 126)
            c = '?';
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (title_bitmap_font[c - 32][row] & (1u << (4 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
        x += 6 * scale;
    }
}
/* Status uses comma-separated indices; insert spaces for the TV display. */
static void draw_dlc(const struct import_status *status, int y, uint32_t color)
{
    char line[sizeof("DLC: ") + IMPORT_DLC_MAX_TEXT * 2];
    size_t n = 0;
    memcpy(line, "DLC: ", 5);
    n = 5;
    if (!status->dlc[0]) {
        memcpy(line + n, "None", sizeof("None"));
    } else {
        for (const char *p = status->dlc; *p; p++) {
            line[n++] = *p;
            if (*p == ',')
                line[n++] = ' ';
        }
        line[n] = 0;
    }
    text(100, y, line, 3, color);
}
static void draw(enum screen sc, struct import_status *s, const char *err, int count, unsigned tick)
{
    const uint32_t white = 0xffe9edf5, muted = 0xffaab8ca, blue = 0xff56b7ed;
    rect(0, 0, 1920, 1080, 0xff101b29);
    text(100, 75, "SAVAGE WIND  /  GAME IMPORT", 5, white);
    char b[1200];
    if (sc == SCAN)
        text(100, 210, "Scanning import folder...", 4, white);
    else if (sc == OFFER) {
        text(100, 210, "Import Breath of the Wild", 5, white);
        if (count < 0)
            text(100, 310, "Cannot read /data/savagewind/import. Check access.", 3, muted);
        else if (!count)
            text(100, 310, "Add NSP or XCI packages to /data/savagewind/import.", 3, muted);
        else {
            snprintf(b, sizeof(b), "%d package(s) found. Contents will be checked before import.", count);
            text(100, 310, b, 3, muted);
        }
        text(100, 920, count > 0 ? "CROSS  Import     CIRCLE  Exit" : "CROSS  Scan again     CIRCLE  Exit", 3,
             white);
    } else if (sc == WORK) {
        const char *label = import_stage_text(s->stage);
        text(100, 210, label ? label : "Starting importer...", 5, white);
        if (!strcmp(s->stage, "awaiting_confirm")) {
            snprintf(b, sizeof(b), "Base: %s\nUpdate: %s\nVersion: %s", s->base_source, s->update_source,
                     s->game_version);
            text(100, 320, b, 3, white);
            draw_dlc(s, 460, white);
            text(100, 920, "CROSS  Confirm     CIRCLE  Cancel", 3, white);
        } else {
            int percent = import_percent(s);
            rect(100, 360, 1500, 30, 0xff37485a);
            if (percent >= 0) {
                rect(100, 360, 15 * percent, 30, blue);
                snprintf(b, sizeof(b), "%llu / %llu bytes   %d%%", (unsigned long long)s->bytes_done,
                         (unsigned long long)s->bytes_total, percent);
            } else {
                rect(100 + (tick % 100) * 14, 360, 100, 30, blue);
                snprintf(b, sizeof(b), "%llu bytes processed  %c", (unsigned long long)s->bytes_done,
                         "|/-\\"[(tick / 4) % 4]);
            }
            text(100, 420, b, 3, white);
            snprintf(b, sizeof(b), "Files: %llu / %llu", (unsigned long long)s->files_done,
                     (unsigned long long)s->files_total);
            text(100, 475, b, 3, muted);
            text(100, 540, s->current, 2, muted);
            text(100, 920, "CIRCLE  Cancel", 3, white);
        }
    } else if (sc == DONE) {
        text(100, 210, "Ready for PC code build", 5, white);
        text(100, 320, "Run the PC helper to build and upload the game code.", 3, muted);
        draw_dlc(s, 400, white);
        text(100, 920, "CIRCLE  Exit", 3, white);
    } else if (sc == CANCELLED) {
        text(100, 210, "Import cancelled", 5, white);
        text(100, 920, "CROSS  Retry     CIRCLE  Exit", 3, white);
    } else {
        text(100, 210, "Import failed", 5, white);
        const char *msg = *err ? err : (*s->message ? s->message : import_error_text(s->error));
        text(100, 320, msg ? msg : "Unknown import error.", 3, white);
        if (*s->error_file) {
            snprintf(b, sizeof(b), "File: %s", s->error_file);
            text(100, 500, b, 3, muted);
        }
        text(100, 920, "CROSS  Retry     CIRCLE  Exit", 3, white);
    }
    struct display_rect full = {0, 0, 1920, 1080};
    display_damage(&full);
    display_present();
}
int main(void)
{
    const char *what = "";
    int r = display_open(&what);
    if (r) {
        char b[256];
        snprintf(b, sizeof(b), "savagewind-import-ui: display %s failed 0x%x\n", what, r);
        sceKernelDebugOutText(0, b);
        sceSystemServiceLoadExec("exit", NULL);
        for (;;)
            sleep(1);
    }
    int user = 0;
    sceUserServiceInitialize(NULL);
    sceUserServiceGetInitialUser(&user);
    scePadInit();
    int pad = scePadOpen(user, 0, 0, NULL);
    pthread_t thread;
    action = RESCAN;
    if (pthread_create(&thread, NULL, worker, NULL))
        set_error("Could not start import worker.");
    uint32_t held = 0;
    unsigned tick = 0;
    for (;;) {
        struct pad_record records[PAD_BATCH_SIZE];
        int n = pad >= 0 ? scePadRead(pad, records, PAD_BATCH_SIZE) : 0;
        for (int i = 0; i < n; i++) {
            uint32_t pressed = records[i].buttons & ~held;
            held = records[i].buttons;
            pthread_mutex_lock(&lock);
            enum screen sc = screen;
            if ((pressed & CROSS) && action == NONE) {
                if (sc == OFFER && package_count <= 0) {
                    screen = SCAN;
                    action = RESCAN;
                } else if (sc == OFFER || sc == ERROR || sc == CANCELLED) {
                    memset(&status, 0, sizeof(status));
                    screen = WORK;
                    action = START;
                } else if (sc == WORK && !strcmp(status.stage, "awaiting_confirm"))
                    action = CONFIRM;
            }
            if (pressed & CIRCLE) {
                if (sc == WORK) {
                    if (action == NONE)
                        action = CANCEL;
                } else {
                    pthread_mutex_unlock(&lock);
                    sceSystemServiceLoadExec("exit", NULL);
                    for (;;)
                        sleep(1);
                }
            }
            pthread_mutex_unlock(&lock);
        }
#ifdef IMPORT_UI_AUTOTEST
        /* Diagnostic builds only (PS5_CFLAGS=-DIMPORT_UI_AUTOTEST): press Cross once on the offer screen and once
         * at the preflight summary so a console run needs no gamepad. Never defined in user builds. */
        {
            static int auto_started, auto_confirmed;
            pthread_mutex_lock(&lock);
            if (!auto_started && screen == OFFER && package_count > 0 && action == NONE) {
                auto_started = 1;
                sceKernelDebugOutText(0, "savagewind-import-ui: AUTOTEST build pressed Cross (start)\n");
                memset(&status, 0, sizeof(status));
                screen = WORK;
                action = START;
            } else if (!auto_confirmed && screen == WORK && !strcmp(status.stage, "awaiting_confirm") &&
                       action == NONE) {
                auto_confirmed = 1;
                sceKernelDebugOutText(0, "savagewind-import-ui: AUTOTEST build pressed Cross (confirm)\n");
                action = CONFIRM;
            }
            pthread_mutex_unlock(&lock);
        }
#endif
        pthread_mutex_lock(&lock);
        enum screen sc = screen;
        struct import_status s = status;
        char err[sizeof(failure)];
        memcpy(err, failure, sizeof(err));
        int count = package_count;
        pthread_mutex_unlock(&lock);
        draw(sc, &s, err, count, tick++);
    }
}
