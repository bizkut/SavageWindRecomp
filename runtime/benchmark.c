// The built-in benchmark (runtime/benchmark.h). The route, in the time since the game was reached (the presents'
// clock, so the same at any frame rate):
//   title screen: A pressed once a second (the title's "press a button", then Continue: the first entry) until it
//   loads; in the game: 20 s for the world to settle (streaming, the first pipelines), then 60 s measured with the
//   camera turning: 5 s still, 20 s right, 3 s up, 3 s down, 20 s left, 9 s still. Link stays where the warp puts
//   him (runtime/services/test_warp.c "plateau"), so nothing the player has done changes the route.
// What is measured is each presented frame of those 60 s (runtime/frametime.c): its length, the render thread's and
// the guest threads' CPU time, its GPU time, and the cause of each hitch; and each present to the display
// (vulkan/device.c: the game's frames and, with frame generation, the generated ones): the presented frame rate and
// the intervals between presents (when they were handed to the display), and the frame generator's GPU time.
#include "benchmark.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "framepace.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#ifdef __PROSPERO__
#include "storage_ps5.h"
#endif

#define S 1000000000ull
enum { WARMUP_S = 20, ROUTE_S = 60, TITLE_TIMEOUT_S = 240, MAX_FRAMES = 16384 };

static int g_on = -1;
bool benchmark_on(void) {
    if (g_on < 0) {
        struct stat st;
        g_on = rt_data_stat("benchmark.txt", &st) == 0;
        if (g_on) LOG("[bench] benchmark.txt: the built-in benchmark runs (Great Plateau camera route, %d s measured), "
                      "then the game closes\n", ROUTE_S);
    }
    return g_on;
}

// ---- files in the data root (the PS5's through the storage broker)
static void make_dirs(const char *path) {  // every directory of path's parent
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    char *slash = strrchr(p, '/');
    if (!slash) return;
    *slash = 0;
#ifdef __PROSPERO__
    rt_storage_mkdirs(p);
#else
    for (char *q = p + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
#endif
}

static bool write_file(const char *path, const void *data, size_t n) {
    make_dirs(path);
#ifdef __PROSPERO__
    return rt_storage_replace(path, data, n) == 0;
#else
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(data, 1, n, f) == n;
    if (f && fclose(f)) ok = false;
    return ok;
#endif
}

static void append_line(const char *path, const char *line) {
    make_dirs(path);
#ifdef __PROSPERO__
    uint64_t size = 0;
    if (rt_storage_stat(path, &size) != 0) rt_storage_replace(path, line, strlen(line));
    else rt_storage_append(path, line, strlen(line));
#else
    FILE *f = fopen(path, "ab");
    if (f) { fputs(line, f); fclose(f); }
#endif
}

static char *read_file(const char *path, size_t *n) {
    FILE *f = rt_data_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (d && fread(d, 1, (size_t)size, f) != (size_t)size) { free(d); d = NULL; }
    fclose(f);
    if (d) d[size] = 0, *n = (size_t)size;
    return d;
}

// ---- the saves: a copy, so that the warp and any autosave never touch the player's
static int copy_tree(const char *src, const char *dst, int *saves) {
    char host[1024];
    if (!rt_data_path(src, host, sizeof host)) return -1;
    DIR *d = opendir(host);
    if (!d) return -1;
    int files = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[512], t[512];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        struct stat st;
        if (rt_data_stat(s, &st)) continue;
        if (S_ISDIR(st.st_mode)) { int k = copy_tree(s, t, saves); if (k > 0) files += k; continue; }
        size_t n = 0;
        char *data = read_file(s, &n);
        if (data && write_file(t, data, n)) files++;
        else LOG("[bench] cannot copy %s\n", s);
        if (!strcmp(e->d_name, "game_data.sav")) (*saves)++;
        free(data);
    }
    closedir(d);
    return files;
}

static const char *g_save_copy;
static bool g_no_save;
const char *benchmark_save_dir(void) {
    if (!benchmark_on()) return NULL;
    static bool done;
    if (!done) {
        done = true;
#ifdef __PROSPERO__
        rt_storage_remove_tree("benchmark/save");
#else
        plat_remove_tree("benchmark/save");
#endif
        int saves = 0, files = copy_tree("save", "benchmark/save", &saves);
        LOG("[bench] the saves copied to benchmark/save: %d files, %d with a game_data.sav\n", files < 0 ? 0 : files, saves);
        if (!saves) g_no_save = true;
        g_save_copy = "benchmark/save";
    }
    return g_save_copy;
}

// ---- the route
enum { B_TITLE_WAIT, B_TITLE, B_LOADING, B_WARMUP, B_ROUTE, B_DONE };
static const char *const state_names[] = {"waiting for the title screen", "title screen", "loading", "settling",
                                          "route", "done"};
static int g_state;
static uint64_t g_t0, g_state_t;  // when the benchmark began, when its state did

static void finish(const char *failure);

static void set_state(int s, uint64_t now) {
    g_state = s, g_state_t = now;
    LOG("[bench] %.1f s: %s\n", (now - g_t0) / 1e9, state_names[s]);
}

bool benchmark_input(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!benchmark_on()) return false;
    uint64_t now = plat_now_ns();
    if (!g_t0) g_t0 = g_state_t = now;
    *buttons = 0, *lx = *ly = *rx = *ry = 0;
    bool title = framepace_title_ui(), loading = framepace_loading_ui();
    static uint64_t away;  // since when the title screen has been gone
    switch (g_state) {
    case B_TITLE_WAIT:
        if (g_no_save) finish("no save to continue (the benchmark continues the latest save)");
        else if (title) set_state(B_TITLE, now);
        else if (now - g_t0 > TITLE_TIMEOUT_S * S) finish("the title screen never came");
        break;
    case B_TITLE:  // A for 150 ms once a second: the title's prompt, then Continue
        if ((now - g_state_t) % S < S * 15 / 100) *buttons = 1;  // (bit 0: A, runtime/hle/input.c)
        if (loading) { set_state(B_LOADING, now); break; }
        if (!title) {
            if (!away) away = now;
            if (now - away > 5 * S) set_state(B_WARMUP, now);  // (in the game without a loading screen)
        } else away = 0;
        if (now - g_state_t > TITLE_TIMEOUT_S * S) finish("Continue did not load the game");
        break;
    case B_LOADING:
        if (!loading && !title) {
            if (!away) away = now;
            if (now - away > 2 * S) set_state(B_WARMUP, now);
        } else away = 0;
        if (now - g_state_t > TITLE_TIMEOUT_S * S) finish("the game did not finish loading");
        break;
    case B_WARMUP:
        if (now - g_state_t > WARMUP_S * S) set_state(B_ROUTE, now);
        break;
    case B_ROUTE: {
        double t = (now - g_state_t) / 1e9;
        if (t >= 5 && t < 25) *rx = 32767;
        else if (t >= 25 && t < 28) *ry = 32767;
        else if (t >= 28 && t < 31) *ry = -32767;
        else if (t >= 31 && t < 51) *rx = -32767;
        break;
    }
    default: break;
    }
    return true;
}

// ---- what was measured
static float g_dt[MAX_FRAMES], g_render[MAX_FRAMES], g_guest[MAX_FRAMES], g_gpu[MAX_FRAMES];
static int g_n, g_ngpu, g_on_time;
static uint64_t g_first_frame;
static struct { char name[48]; int n; } g_causes[16];
static int g_ncauses, g_hitches;

void benchmark_frame(uint64_t frame, uint64_t dt_ns, uint64_t render_ns, uint64_t guest_ns, const char *hitch) {
    if (g_on != 1) return;
    if (g_state != B_ROUTE) return;
    if (plat_now_ns() - g_state_t >= ROUTE_S * S) { finish(NULL); return; }
    if (!g_n) g_first_frame = frame;
    if (g_n == MAX_FRAMES) return;
    uint64_t period = framepace_period_ns();
    g_dt[g_n] = (float)(dt_ns / 1e6), g_render[g_n] = (float)(render_ns / 1e6), g_guest[g_n] = (float)(guest_ns / 1e6);
    g_on_time += dt_ns <= period + period / 20;
    g_n++;
    if (!hitch) return;
    g_hitches++;
    char name[48];
    size_t k = strcspn(hitch, "(:");  // ("shader compile (render thread, ...)" -> "shader compile")
    snprintf(name, sizeof name, "%.*s", (int)(k && hitch[k - 1] == ' ' ? k - 1 : k), hitch);
    int i = 0;
    while (i < g_ncauses && strcmp(g_causes[i].name, name)) i++;
    if (i == g_ncauses && g_ncauses < 16) snprintf(g_causes[g_ncauses++].name, sizeof g_causes[0].name, "%s", name);
    if (i < 16) g_causes[i].n++;
}

void benchmark_gpu(uint64_t frame, uint64_t ns) {
    if (g_on != 1 || !g_n || frame < g_first_frame || g_ngpu == MAX_FRAMES) return;
    g_gpu[g_ngpu++] = (float)(ns / 1e6);
}

// ---- presents: their times (the present thread writes, finish reads up to the count it publishes) and frame
// generation's GPU time (the render thread)
enum { MAX_PRESENTS = 2 * MAX_FRAMES };
static uint64_t g_present_t[MAX_PRESENTS];
static int g_npresents, g_npresents_gen;  // (g_npresents: release / acquire)
static float g_fg_gpu[MAX_FRAMES];
static int g_nfg;

static bool measuring(void) {
    return __atomic_load_n(&g_on, __ATOMIC_RELAXED) == 1 && __atomic_load_n(&g_state, __ATOMIC_RELAXED) == B_ROUTE;
}

void benchmark_present(bool generated) {
    if (!measuring()) return;
    int n = __atomic_load_n(&g_npresents, __ATOMIC_RELAXED);
    if (n == MAX_PRESENTS) return;
    g_present_t[n] = plat_now_ns();
    g_npresents_gen += generated;
    __atomic_store_n(&g_npresents, n + 1, __ATOMIC_RELEASE);
}

void benchmark_fg_gpu(uint64_t ns) {
    if (!measuring() || g_nfg == MAX_FRAMES) return;
    g_fg_gpu[g_nfg++] = (float)(ns / 1e6);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
typedef struct { double mean, p50, p95, p99, max; } Dist;
static Dist dist(const float *v, int n) {
    Dist d = {0};
    if (n <= 0) return d;
    float *s = malloc(sizeof *s * (size_t)n);
    memcpy(s, v, sizeof *s * (size_t)n);
    qsort(s, (size_t)n, sizeof *s, cmp_float);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    d.mean = sum / n, d.p50 = s[n / 2], d.p95 = s[(int)(n * 0.95)], d.p99 = s[(int)(n * 0.99)], d.max = s[n - 1];
    free(s);
    return d;
}

static void finish(const char *failure) {
    if (g_state == B_DONE) return;
    g_state = B_DONE;
    char text[4096];
    size_t at = 0;
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    double mean = 0, p95 = 0, presented_fps = 0, fg_ms = 0;
    if (failure) {
        at += (size_t)snprintf(text + at, sizeof text - at, "Savage Wind benchmark, %s: not run: %s\n", when, failure);
        LOG("[bench] not run: %s\n", failure);
    } else {
        Dist f = dist(g_dt, g_n), r = dist(g_render, g_n), g = dist(g_guest, g_n), u = dist(g_gpu, g_ngpu);
        mean = f.mean, p95 = f.p95;
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "Savage Wind benchmark, %s: Great Plateau camera route, %d s measured after %d s settling\n"
                               "frames %d: frame ms mean %.3f p50 %.3f p95 %.3f p99 %.3f max %.1f; %.1f%% on time; %d hitches",
                               when, ROUTE_S, WARMUP_S, g_n, f.mean, f.p50, f.p95, f.p99, f.max,
                               g_n ? 100.0 * g_on_time / g_n : 0.0, g_hitches);
        for (int i = 0; i < g_ncauses; i++)
            at += (size_t)snprintf(text + at, sizeof text - at, "%s %s %d", i ? "," : ":", g_causes[i].name, g_causes[i].n);
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "\ncpu ms per frame: render thread mean %.2f p95 %.2f max %.1f; guest threads (summed) mean %.2f "
                               "p95 %.2f max %.1f\ngpu ms per frame (%d frames): mean %.2f p95 %.2f max %.1f\n",
                               r.mean, r.p95, r.max, g.mean, g.p95, g.max, g_ngpu, u.mean, u.p95, u.max);
        // the presents: their rate, the intervals between them (when each was handed to the display; at 119.88 Hz one
        // refresh is 8.34 ms) and frame generation's GPU time per generated frame
        int np = __atomic_load_n(&g_npresents, __ATOMIC_ACQUIRE), niv = np > 1 ? np - 1 : 0;
        double span = niv ? (double)(g_present_t[np - 1] - g_present_t[0]) / 1e9 : 0, dt_sum = 0;
        for (int i = 0; i < g_n; i++) dt_sum += g_dt[i];
        presented_fps = span > 0 ? niv / span : 0;
        double game_fps = dt_sum > 0 ? g_n / (dt_sum / 1e3) : 0;
        static float iv[MAX_PRESENTS];
        static const double edge[5] = {6.5, 10.0, 15.0, 18.4, 25.0};
        int bins[6] = {0};
        for (int i = 0; i < niv; i++) {
            iv[i] = (float)((g_present_t[i + 1] - g_present_t[i]) / 1e6);
            int k = 0;
            while (k < 5 && iv[i] >= edge[k]) k++;
            bins[k]++;
        }
        Dist pi = dist(iv, niv), fd = dist(g_fg_gpu, g_nfg);
        double pct = niv ? 100.0 / niv : 0;
        fg_ms = fd.mean;
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "presents %d: %.1f presented fps (%d in frame generation's slot); game frames %.1f fps\n"
                               "present intervals ms: mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.1f; under 6.5 %.1f%%, "
                               "6.5-10 %.1f%%, 10-15 %.1f%%, 15-18.4 %.1f%%, 18.4-25 %.1f%%, over 25 %.1f%%\n",
                               np, presented_fps, g_npresents_gen, game_fps, pi.mean, pi.p50, pi.p95, pi.p99, pi.max,
                               bins[0] * pct, bins[1] * pct, bins[2] * pct, bins[3] * pct, bins[4] * pct, bins[5] * pct);
        if (g_nfg)
            at += (size_t)snprintf(text + at, sizeof text - at,
                                   "frame generation gpu ms per generated frame (%d frames): mean %.3f p50 %.3f p95 %.3f max %.3f\n",
                                   g_nfg, fd.mean, fd.p50, fd.p95, fd.max);
        else
            at += (size_t)snprintf(text + at, sizeof text - at, "frame generation: %s\n",
                                   g_npresents_gen ? "on, its GPU time not measured" : "off");
        // the run before this one, from its last.txt
        size_t n = 0;
        char *prev = read_file("benchmark/last.txt", &n);
        double pm = 0, pp = 0;
        const char *line = prev ? strstr(prev, "result mean_ms=") : NULL;
        if (line && sscanf(line, "result mean_ms=%lf p95_ms=%lf", &pm, &pp) == 2 && pm > 0 && pp > 0) {
            double dm = 100 * (mean - pm) / pm, dp = 100 * (p95 - pp) / pp;
            at += (size_t)snprintf(text + at, sizeof text - at,
                                   "previous run: mean %.3f p95 %.3f; this one %+.2f%% / %+.2f%%: %s within 2%%\n", pm, pp, dm, dp,
                                   dm <= 2 && dm >= -2 && dp <= 2 && dp >= -2 ? "agree" : "do not agree");
        } else {
            at += (size_t)snprintf(text + at, sizeof text - at, "previous run: none\n");
        }
        free(prev);
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "result mean_ms=%.3f p95_ms=%.3f p99_ms=%.3f on_time=%.1f hitches=%d render_ms=%.2f guest_ms=%.2f "
                               "gpu_ms=%.2f frames=%d presented_fps=%.1f game_fps=%.1f present_iv_p95_ms=%.2f fg_gpu_ms=%.3f\n",
                               f.mean, f.p95, f.p99, g_n ? 100.0 * g_on_time / g_n : 0.0, g_hitches, r.mean, g.mean, u.mean,
                               g_n, presented_fps, game_fps, pi.p95, fd.mean);
        write_file("benchmark/last.txt", text, at < sizeof text ? at : sizeof text - 1);
        for (char *p = text, *e; *p; p = e + 1) {  // (the summary in the log too, one line each)
            e = strchr(p, '\n');
            if (!e) break;
            LOG("[bench] %.*s\n", (int)(e - p), p);
        }
    }
    char hist[256];
    snprintf(hist, sizeof hist, "%s %s mean_ms=%.3f p95_ms=%.3f frames=%d presented_fps=%.1f fg_gpu_ms=%.3f\n", when,
             failure ? "failed" : "done", mean, p95, failure ? 0 : g_n, presented_fps, fg_ms);
    append_line("benchmark/history.txt", hist);
    LOG("[bench] summary in benchmark/last.txt (and benchmark/history.txt): the game closes\n");
    fflush(stderr);
    exit(0);
}
