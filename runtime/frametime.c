// Frame time and the causes of long frames (runtime/frametime.h).
#include "frametime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "benchmark.h"
#include "framepace.h"
#include "kernel.h"
#include "rt.h"
#ifdef __PROSPERO__
#include "nvn/vulkan/record.h"
#include "nvn/vulkan/rprof.h"
#endif

#ifdef __PROSPERO__
#include "ps5/dynamic_resolution.h"
#endif
uint64_t g_ft[FT_COUNT];
static uint32_t present_handle;  // the guest thread that presents (0: none yet)

void ft_present_thread(void) {
    uint32_t h = kernel_current_thread_handle();
    if (h && __atomic_load_n(&present_handle, __ATOMIC_RELAXED) != h) __atomic_store_n(&present_handle, h, __ATOMIC_RELAXED);
}

// ---- GPU: a submission's time goes to the frame it was recorded for. Results arrive in submission order, so a
// submission of a later frame closes the frames before it.
static uint64_t gpu_frame = ~0ull, gpu_ns;          // the frame being added up
static uint64_t gpu_win_sum, gpu_win_max, gpu_win_n;  // closed frames of this window
static uint64_t gpu_min_sum, gpu_min_n;               // ... of this minute ([perf])
#ifdef __PROSPERO__
// The GPU timestamp query arrives a few submissions later. Pair it with the same presented-frame number rather
// than attributing the most recently resolved GPU time to a different frame.
typedef struct {
    uint64_t frame, dt, render, recorder_ticks, gpu, gpu_wait, recorder_wait;
    uint64_t rp[RP_COUNT], draws, driver_ticks, driver_calls;
    bool have_frame, have_gpu, late;
} LateSample;
static LateSample late_samples[256];
static unsigned late_logged, normal_logged;
static void late_report(LateSample *s) {
    if (!s->have_frame || !s->have_gpu || (s->late ? late_logged >= 64 : normal_logged >= 16)) return;
    double k = rprof_tick_ns() / 1e6;
    LOG("[lateprofile] %s frame %llu dt %.3f render %.3f recorder_window %.3f gpu %.3f gpu_wait %.3f recorder_wait %.3f"
        " draws %llu pre %.3f pass %.3f vset %.3f tex %.3f fset %.3f state %.3f list %.3f mvec %.3f match %.3f"
        " scene %.3f fsr %.3f driver %.3f calls %llu ms\n",
        s->late ? "late" : "normal", (unsigned long long)s->frame, s->dt / 1e6, s->render / 1e6,
        s->recorder_ticks * k, s->gpu / 1e6, s->gpu_wait / 1e6, s->recorder_wait / 1e6,
        (unsigned long long)s->draws, s->rp[RP_D_PRE] * k, s->rp[RP_D_PASS] * k, s->rp[RP_D_VSET] * k,
        s->rp[RP_D_TEX] * k, s->rp[RP_D_FSET] * k, s->rp[RP_OP_STATE] * k, s->rp[RP_OP_LIST] * k,
        s->rp[RP_MVEC_RECORD] * k, s->rp[RP_MVEC_MATCH] * k, s->rp[RP_SCENE_POST] * k,
        s->rp[RP_FSR3_DISPATCH] * k, s->driver_ticks * k, (unsigned long long)s->driver_calls);
    if (s->late) late_logged++; else normal_logged++;
    s->have_frame = false;
}
static LateSample *late_slot(uint64_t frame) {
    LateSample *s = &late_samples[frame % 256];
    if ((!s->have_frame && !s->have_gpu) || s->frame != frame) {
        memset(s, 0, sizeof *s);
        s->frame = frame;
    }
    return s;
}
#endif

static void gpu_close(void) {
    if (gpu_frame == ~0ull) return;
    gpu_win_sum += gpu_ns, gpu_win_n++;
    if (gpu_ns > gpu_win_max) gpu_win_max = gpu_ns;
    gpu_min_sum += gpu_ns, gpu_min_n++;
    benchmark_gpu(gpu_frame, gpu_ns);
#ifdef __PROSPERO__
    if (g_rprof && benchmark_segment() >= 0) {
        LateSample *s = late_slot(gpu_frame);
        s->gpu = gpu_ns;
        s->have_gpu = true;
        late_report(s);
    }
#endif
    #ifdef __PROSPERO__
    dynamic_resolution_gpu(gpu_ns);
    #endif
}

void ft_gpu(uint64_t frame, uint64_t ns) {
    if (frame != gpu_frame) {
        gpu_close();
        gpu_frame = frame, gpu_ns = 0;
    }
    gpu_ns += ns;
}

// ---- threads: each frame, the difference of every guest thread's CPU time and waits since the last frame
#define THREADS 128
typedef struct { uint32_t handle; int prio; uint64_t entry, cpu; KTimes t; } Snap;
typedef struct { uint32_t handle; int prio; uint64_t entry, cpu; KTimes t; } Delta;
static Snap prev[THREADS];
static int nprev;
static Delta delta[THREADS];
static int ndelta;
static KThreadTimes raw[THREADS];

static void threads_step(void) {
    int n = kernel_thread_times(raw, THREADS);
    static Snap cur[THREADS];
    ndelta = 0;
    for (int i = 0; i < n; i++) {
        Snap *c = &cur[i];
        c->handle = raw[i].handle, c->prio = raw[i].prio, c->entry = raw[i].entry, c->t = raw[i].times;
        c->cpu = raw[i].times.cpu_ns;
        // (the same order as last frame unless a thread started: new threads come first)
        const Snap *p = i < nprev && prev[i].handle == c->handle ? &prev[i] : NULL;
        for (int k = 0; k < nprev && !p; k++) if (prev[k].handle == c->handle) p = &prev[k];
        if (!p) continue;  // (started since the last frame: from the next frame on)
        Delta *d = &delta[ndelta++];
        d->handle = c->handle, d->prio = c->prio, d->entry = c->entry;
        d->cpu = c->cpu >= p->cpu ? c->cpu - p->cpu : 0;
        const uint64_t *a = (const uint64_t *)&c->t, *b = (const uint64_t *)&p->t;
        uint64_t *o = (uint64_t *)&d->t;
        for (size_t k = 0; k < sizeof c->t / sizeof(uint64_t); k++) o[k] = a[k] >= b[k] ? a[k] - b[k] : 0;
    }
    memcpy(prev, cur, sizeof *cur * (size_t)n);
    nprev = n;
}

static const Delta *delta_of(uint32_t handle) {
    for (int i = 0; i < ndelta; i++) if (delta[i].handle == handle) return &delta[i];
    return NULL;
}

// ---- the 2 s window
typedef struct { uint32_t handle; uint64_t sum, max; } WinThread;
static WinThread win_threads[THREADS];
static int nwin_threads;
static uint64_t win_frames, win_dt_sum, win_dt_max, win_on_time, win_hitches;
static uint64_t win_render_sum, win_render_max, win_guest_sum, win_guest_max;

static void win_thread_add(uint32_t handle, uint64_t cpu) {
    int i = 0;
    while (i < nwin_threads && win_threads[i].handle != handle) i++;
    if (i == nwin_threads) {
        if (nwin_threads == THREADS) return;
        win_threads[nwin_threads++] = (WinThread){handle, 0, 0};
    }
    win_threads[i].sum += cpu;
    if (cpu > win_threads[i].max) win_threads[i].max = cpu;
}

// ---- [perf]: frame times per minute in 0.25 ms bins up to 100 ms, the last 10 minutes kept
#define BINS 401
#define MINUTES 10
typedef struct { uint32_t bin[BINS]; uint64_t frames, on_time, hitches, render, guest, gpu, gpu_n; } Minute;
static Minute minutes[MINUTES];
static int minute_at, minutes_done;
static uint64_t minute_t0;

static double bins_percentile(const uint32_t *bin, uint64_t frames, double q) {
    uint64_t want = (uint64_t)((double)frames * q), seen = 0;
    for (int i = 0; i < BINS; i++) {
        seen += bin[i];
        if (seen > want) return i == BINS - 1 ? 100.0 : (i + 1) * 0.25;
    }
    return 100.0;
}

static void perf_line(const char *what, const Minute *m) {
    if (!m->frames) return;
    LOG("[perf] %s: %llu frames, %.1f%% on time, frame ms p50 %.2f p95 %.2f p99 %.2f, %llu hitches; per frame: render "
        "thread cpu %.2f ms, guest threads cpu %.2f ms, gpu %.2f ms\n", what, (unsigned long long)m->frames,
        100.0 * (double)m->on_time / (double)m->frames, bins_percentile(m->bin, m->frames, 0.50),
        bins_percentile(m->bin, m->frames, 0.95), bins_percentile(m->bin, m->frames, 0.99), (unsigned long long)m->hitches,
        (double)m->render / (double)m->frames / 1e6, (double)m->guest / (double)m->frames / 1e6,
        m->gpu_n ? (double)m->gpu / (double)m->gpu_n / 1e6 : 0.0);
}

static void perf_minute(uint64_t now) {
    Minute *m = &minutes[minute_at];
    m->gpu = gpu_min_sum, m->gpu_n = gpu_min_n;
    gpu_min_sum = gpu_min_n = 0;
    perf_line("last 60 s", m);
    minute_at = (minute_at + 1) % MINUTES;
    if (minutes_done < MINUTES) minutes_done++;
    if (minutes_done == MINUTES) {  // (the last 10 minutes, M6's window)
        static Minute all;
        memset(&all, 0, sizeof all);
        for (int k = 0; k < MINUTES; k++) {
            const Minute *s = &minutes[k];
            for (int i = 0; i < BINS; i++) all.bin[i] += s->bin[i];
            all.frames += s->frames, all.on_time += s->on_time, all.hitches += s->hitches;
            all.render += s->render, all.guest += s->guest, all.gpu += s->gpu, all.gpu_n += s->gpu_n;
        }
        perf_line("last 10 min", &all);
    }
    memset(&minutes[minute_at], 0, sizeof minutes[minute_at]);
    minute_t0 = now;
}

// ---- hitches
static const char *const ui_names[] = {"play", "title screen", "loading screen", "menu"};
static int ui_now(void) {
    return framepace_title_ui() ? 1 : framepace_loading_ui() ? 2 : framepace_menu_ui() ? 3 : 0;
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

// What the frame went to: the render thread's side when it was busy for most of the frame, else what the presenting
// guest thread did (its own work, a read, or waiting for another guest thread: then that thread's work).
static void hitch_cause(char *out, size_t n, uint64_t dt, uint64_t rcpu, const uint64_t *d, const Delta *pt) {
    // builds the render thread ran itself (synchronous shaders), shared out between shaders and pipelines as the
    // builds' own times (which also count builds on worker threads) are
    uint64_t built = d[FT_SHADER_NS] + d[FT_PIPELINE_NS], in = d[FT_BUILD_INLINE_NS];
    uint64_t shader = built ? (uint64_t)((double)in * (double)d[FT_SHADER_NS] / (double)built) : 0, pipe = in - shader;
    uint64_t inline_work = in + d[FT_TEXTURE_NS];
    uint64_t busy = rcpu + d[FT_GPU_WAIT_NS] + d[FT_BUILD_WAIT_NS] + d[FT_ACQUIRE_NS];
    if (busy * 2 > dt || !pt) {
        struct { const char *name; uint64_t ns; } c[] = {
            {"shader compile", shader + d[FT_BUILD_WAIT_NS]}, {"pipeline creation", pipe},
            {"texture upload", d[FT_TEXTURE_NS]}, {"GPU wait", d[FT_GPU_WAIT_NS]},
            {"swapchain acquire", d[FT_ACQUIRE_NS]},
            {"command translation", rcpu > inline_work ? rcpu - inline_work : 0},
        };
        int best = 0;
        for (int i = 1; i < (int)(sizeof c / sizeof c[0]); i++) if (c[i].ns > c[best].ns) best = i;
        snprintf(out, n, "%s (render thread, %.1f ms)", c[best].name, ms(c[best].ns));
        return;
    }
    struct { const char *name; uint64_t ns; } c[] = {
        {"game CPU", pt->cpu}, {"package read", pt->t.romfs_ns}, {"guest-thread wait", pt->t.wait_ns},
        {"core wait", pt->t.core_ns}, {"render queue / GPU wait", pt->t.host_ns}, {"pacing", pt->t.sleep_ns},
    };
    int best = 0;
    for (int i = 1; i < (int)(sizeof c / sizeof c[0]); i++) if (c[i].ns > c[best].ns) best = i;
    if (best == 2) {  // waiting for another guest thread: the one that worked most meanwhile
        const Delta *o = NULL;
        for (int i = 0; i < ndelta; i++)
            if (delta[i].handle != pt->handle && (!o || delta[i].cpu + delta[i].t.romfs_ns > o->cpu + o->t.romfs_ns)) o = &delta[i];
        if (o && o->t.romfs_ns > o->cpu)
            snprintf(out, n, "package read on guest thread %x (%.1f ms, %llu reads, %.1f MB; thread %x waited %.1f ms)",
                     o->handle, ms(o->t.romfs_ns), (unsigned long long)o->t.romfs_reads, o->t.romfs_bytes / 1048576.0,
                     pt->handle, ms(pt->t.wait_ns));
        else if (o)
            snprintf(out, n, "guest-thread wait: thread %x waited %.1f ms, thread %x ran %.1f ms", pt->handle,
                     ms(pt->t.wait_ns), o->handle, ms(o->cpu));
        else snprintf(out, n, "guest-thread wait (%.1f ms)", ms(pt->t.wait_ns));
        return;
    }
    snprintf(out, n, "%s (guest thread %x, %.1f ms)", c[best].name, pt->handle, ms(c[best].ns));
}

static void hitch_log(uint64_t frame, uint64_t dt, uint64_t rcpu, const uint64_t *d, const Delta *pt, const char *cause) {
    static int logged;
    if (logged >= 300) {
        if (logged++ == 300) LOG("[hitch] 300 hitches logged: the rest are only counted (the [vk] lines)\n");
        return;
    }
    logged++;
    char threads[640] = "";
    // the guest threads that did most in it (CPU, reads), at most four
    int used[4] = {-1, -1, -1, -1};
    size_t at = 0;
    for (int k = 0; k < 4 && at < sizeof threads; k++) {
        int best = -1;
        for (int i = 0; i < ndelta; i++) {
            if (i == used[0] || i == used[1] || i == used[2]) continue;
            uint64_t v = delta[i].cpu + delta[i].t.romfs_ns;
            if (v >= 1000000 && (best < 0 || v > delta[best].cpu + delta[best].t.romfs_ns)) best = i;
        }
        if (best < 0) break;
        used[k] = best;
        const Delta *t = &delta[best];
        at += (size_t)snprintf(threads + at, sizeof threads - at, "%s%x (prio %d) cpu %.1f wait %.1f core %.1f", k ? "; " : "",
                               t->handle, t->prio, ms(t->cpu), ms(t->t.wait_ns), ms(t->t.core_ns));
        if (t->t.romfs_reads && at < sizeof threads)
            at += (size_t)snprintf(threads + at, sizeof threads - at, " read %.1f (%llu, %.1f MB)", ms(t->t.romfs_ns),
                                   (unsigned long long)t->t.romfs_reads, t->t.romfs_bytes / 1048576.0);
        if (at >= sizeof threads) break;
    }
    char present[200] = "no presenting guest thread";
    if (pt)
        snprintf(present, sizeof present, "present thread %x: cpu %.1f wait %.1f core %.1f sleep %.1f backend %.1f read %.1f",
                 pt->handle, ms(pt->cpu), ms(pt->t.wait_ns), ms(pt->t.core_ns), ms(pt->t.sleep_ns), ms(pt->t.host_ns),
                 ms(pt->t.romfs_ns));
    LOG("[hitch] frame %llu: %.1f ms in %s, cause: %s | render thread cpu %.1f ms: compiles %.1f ms (%llu), pipelines %.1f "
        "ms (%llu), build waits %.1f, textures %.1f ms (%.1f MB), GPU waits %.1f, present %.1f (acquire %.1f), uploads "
        "%.1f MB | %s | threads: %s\n", (unsigned long long)frame, ms(dt), ui_names[ui_now()], cause, ms(rcpu),
        ms(d[FT_SHADER_NS]), (unsigned long long)d[FT_SHADERS], ms(d[FT_PIPELINE_NS]), (unsigned long long)d[FT_PIPELINES],
        ms(d[FT_BUILD_WAIT_NS]), ms(d[FT_TEXTURE_NS]), d[FT_TEXTURE_BYTES] / 1048576.0, ms(d[FT_GPU_WAIT_NS]),
        ms(d[FT_PRESENT_NS]), ms(d[FT_ACQUIRE_NS]), d[FT_UPLOAD_BYTES] / 1048576.0, present, threads[0] ? threads : "none over 1 ms");
}

// ---- each presented frame
void ft_frame(void) {
    static bool started;
    static uint64_t last_t, last_render, last_ft[FT_COUNT], frame;
    uint64_t now = plat_now_ns(), rc = plat_thread_cpu_ns();
    if (!started) {
        started = true;
        LOG("[frametime] thread CPU times %s (a guest thread's as of its latest wait); a frame over %.1f ms (1.5 "
            "periods, at least 25) is logged as a hitch\n", rc ? "available" : "unavailable (CPU times read 0)",
            framepace_period_ns() * 1.5 / 1e6 > 25 ? framepace_period_ns() * 1.5 / 1e6 : 25.0);
        minute_t0 = now;
    }
    uint64_t d[FT_COUNT];
    for (int i = 0; i < FT_COUNT; i++) {
        uint64_t v = __atomic_load_n(&g_ft[i], __ATOMIC_RELAXED);
        d[i] = v - last_ft[i];
        last_ft[i] = v;
    }
    threads_step();
    frame++;
    if (!last_t) { last_t = now, last_render = rc; return; }
    uint64_t dt = now - last_t, rcpu = rc - last_render;
    last_t = now, last_render = rc;
    uint64_t guest = 0;
    for (int i = 0; i < ndelta; i++) { guest += delta[i].cpu; win_thread_add(delta[i].handle, delta[i].cpu); }
    uint64_t period = framepace_period_ns();
    static uint64_t thr_env = ~0ull;
    if (thr_env == ~0ull) thr_env = getenv("SAVAGEWIND_HITCH_MS") ? (uint64_t)atoi(getenv("SAVAGEWIND_HITCH_MS")) * 1000000ull : 0;
    uint64_t thr = thr_env ? thr_env : period * 3 / 2 > 25000000 ? period * 3 / 2 : 25000000;
    bool on_time = dt <= period + period / 20, hitch = dt > thr;
#ifdef __PROSPERO__
    if (g_rprof) {
        extern uint64_t renderer_present_count(void);
        extern uint64_t g_vk_driver_ticks[], g_vk_driver_calls[];
        extern const int g_vk_driver_count;
        static uint64_t last_rp[RP_COUNT], last_draws, last_driver, last_driver_calls;
        uint64_t driver = 0, calls = 0;
        for (int i = 0; i < g_vk_driver_count; i++) driver += g_vk_driver_ticks[i], calls += g_vk_driver_calls[i];
        static RecStats last_rec;
        RecStats rec;
        rec_stats(&rec);
        if (benchmark_segment() >= 0) {
            LateSample *s = late_slot(renderer_present_count());
            s->dt = dt; s->render = rcpu; s->gpu_wait = d[FT_GPU_WAIT_NS];
            s->recorder_ticks = rec.busy_ticks - last_rec.busy_ticks;
            s->recorder_wait = rec.drain_ns - last_rec.drain_ns + rec.seq_wait_ns - last_rec.seq_wait_ns +
                               rec.full_ns - last_rec.full_ns;
            for (int i = 0; i < RP_COUNT; i++) s->rp[i] = g_rp[i] - last_rp[i];
            s->draws = g_rp_n[RP_OP_DRAW] - last_draws;
            s->driver_ticks = driver - last_driver;
            s->driver_calls = calls - last_driver_calls;
            s->have_frame = true; s->late = !on_time;
            late_report(s);
        }
        memcpy(last_rp, g_rp, sizeof last_rp);
        last_draws = g_rp_n[RP_OP_DRAW]; last_driver = driver; last_driver_calls = calls;
        last_rec = rec;
    }
#endif
    win_frames++, win_dt_sum += dt, win_on_time += on_time, win_hitches += hitch;
    if (dt > win_dt_max) win_dt_max = dt;
    win_render_sum += rcpu, win_guest_sum += guest;
    if (rcpu > win_render_max) win_render_max = rcpu;
    if (guest > win_guest_max) win_guest_max = guest;
    Minute *m = &minutes[minute_at];
    uint64_t b = dt / 250000;
    m->bin[b < BINS ? b : BINS - 1]++;
    m->frames++, m->on_time += on_time, m->hitches += hitch, m->render += rcpu, m->guest += guest;
    char cause[256] = "";
    if (hitch) {
        uint32_t ph = __atomic_load_n(&present_handle, __ATOMIC_RELAXED);
        const Delta *pt = ph ? delta_of(ph) : NULL;
        hitch_cause(cause, sizeof cause, dt, rcpu, d, pt);
        hitch_log(frame, dt, rcpu, d, pt, cause);
    }
    benchmark_frame(frame, dt, rcpu, guest, hitch ? cause : NULL);
    if (now - minute_t0 >= 60000000000ull) perf_minute(now);
}

void ft_window(char *out, size_t n) {
    // the guest threads with the most CPU time in the window, the presenting one marked
    uint32_t ph = __atomic_load_n(&present_handle, __ATOMIC_RELAXED);
    double f = win_frames ? (double)win_frames : 1;
    size_t at = (size_t)snprintf(out, n, "frame %.1f ms (max %.1f), %.1f%% on time, %llu hitches; cpu ms/frame: render %.1f "
                                 "(max %.1f), guest %.1f (max %.1f):", ms(win_dt_sum) / f, ms(win_dt_max),
                                 100.0 * (double)win_on_time / f, (unsigned long long)win_hitches, ms(win_render_sum) / f,
                                 ms(win_render_max), ms(win_guest_sum) / f, ms(win_guest_max));
    bool used[THREADS] = {false};
    for (int k = 0; k < 4 && at < n; k++) {
        int best = -1;
        for (int i = 0; i < nwin_threads; i++)
            if (!used[i] && (best < 0 || win_threads[i].sum > win_threads[best].sum)) best = i;
        if (best < 0 || win_threads[best].sum < 100000 * win_frames) break;
        used[best] = true;
        at += (size_t)snprintf(out + at, n - at, " %x%s %.1f (max %.1f)", win_threads[best].handle,
                               win_threads[best].handle == ph ? "*" : "", ms(win_threads[best].sum) / f, ms(win_threads[best].max));
    }
    if (at < n)
        snprintf(out + at, n - at, "; gpu ms/frame %.1f (max %.1f, %llu frames)", gpu_win_n ? ms(gpu_win_sum) / (double)gpu_win_n : 0.0,
                 ms(gpu_win_max), (unsigned long long)gpu_win_n);
    win_frames = win_dt_sum = win_dt_max = win_on_time = win_hitches = 0;
    win_render_sum = win_render_max = win_guest_sum = win_guest_max = 0;
    gpu_win_sum = gpu_win_max = gpu_win_n = 0;
    nwin_threads = 0;
}
