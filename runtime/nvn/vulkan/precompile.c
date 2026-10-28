// Shader precompilation (runtime/nvn/precompile.h). Every shader binary of the game's RomFS (shader_archive.c) is
// translated and compiled to SPIR-V with the keys the game will give it, by worker threads, into the caches the game
// reads (cache/xlate, cache/spirv); then pipelines are built for them into the pipeline cache (exec.c).
//
// Keys (vk_key_normalize: only what a shader uses of them):
//  - compute: the shared memory size from its control section; nothing else varies.
//  - fragment: which of its inputs the vertex shader provides - all of them (in BOTW's archives each program's vertex
//    shader writes exactly what its fragment shader reads) - and the kind of each color target it writes: float (no
//    integer targets met).
//  - vertex: the window origin and depth mode (game-wide), and the kind of each attribute it reads: float, int or
//    uint. That depends on the models' vertex formats; in BOTW it is the same for an attribute across one archive:
//    learned from the shader key log of this player's sessions (vk_keylog_read), else what the scripted test walks
//    met (kinds[] below), else float.
// Keys of the shader key log for the same binary are added as they are: what this player's game used.
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "../../kernel.h"
#include "../../platform.h"
#include "../../paths.h"
#include "../precompile.h"
#include "../shader_archive.h"
#include "cache_pack.h"
#include "shaders.h"
#include "vk.h"
#ifdef __PROSPERO__
#include "../../settings.h"
#include "../../storage_ps5.h"
#endif

bool shader_precompile_available(void) { return true; }

// Vertex attributes that are integers, by archive (the rest are float): what the scripted test walks met.
static const struct { const char *archive; int attr, kind; } kinds[] = {
    {"/uking_mat.product.sbfsha", 6, 2},    // uint (the bone indices of skinned models)
    {"/uking_grass.product.sbfsha", 2, 4},  // uint read as float
};

typedef struct { int shader; ShaderKey key; } Job;
typedef struct { int vs, fs, cs; } Pair;  // (jobs: the predicted key's; vs or fs alone: that shader's libraries)
typedef struct { int shader; char why[200]; } Failure;

typedef struct {
    ShaderPrecompile *p;
    SaSet *set;
    Job *jobs;
    int njobs;
    Pair *pairs;
    int npairs;
    _Atomic int next, made, cached, failed, built;
    Failure fails[64];            // the first failed translations (logged together after the step)
    bool shared_heap;             // the workers allocate from the heap's shared arenas (the scaling check)
    // the pipelines step: the driver's compiles share a memory budget (pipe_take)
    int *order;                   // pairs, largest shader first
    int64_t *need_mb;             // per pair: what its compile is estimated to take
    int next_big, next_small;     // still to take: order[next_big .. next_small]
    int64_t used_mb, budget_mb;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} Work;

// Where the workers' time goes (shader_precompile_log_window): items finished; their own work (wall, and the thread's
// CPU in it: much less CPU than wall is waiting inside, on locks, memory or files); reading what the step before
// made; waiting for room in the memory budget (pipe_take). And, clock-free, the core each item finished on (the
// PS5's sceKernelGetCurrentCpu) and each worker's affinity mask (the PS5's thread CPU clocks are not trusted: M6).
static struct {
    _Atomic uint64_t items, work_ns, work_cpu_ns, read_ns, wait_ns;
    _Atomic uint32_t core[32];
    _Atomic uint64_t mask[33];  // per worker (+1: the mask, so 0 is "not known")
    _Atomic int nmask;
} g_st;

#ifdef __PROSPERO__
int sceKernelGetCurrentCpu(void);
int pthread_getaffinity_np(pthread_t thread, size_t size, void *set);
static int current_cpu(void) { return sceKernelGetCurrentCpu(); }
static uint64_t thread_mask(void) {
    uint64_t m[2] = {0, 0};  // (cpuset_t: 128 bits)
    return pthread_getaffinity_np(pthread_self(), sizeof m, m) ? 0 : m[0];
}
#else
static int current_cpu(void) { return -1; }
static uint64_t thread_mask(void) { return 0; }
#endif

static void note_core(void) {
    int c = current_cpu();
    if (c >= 0 && c < 32) atomic_fetch_add(&g_st.core[c], 1);
}

static void note_mask(void) {
    int i = atomic_fetch_add(&g_st.nmask, 1);
    if (i < 32) atomic_store(&g_st.mask[i], thread_mask() + 1);
}

static void *worker(void *arg) {
    Work *w = arg;
    maxwell_gmem_base = HEAP_BASE;  // (as the game translates: global memory relative to the guest heap)
    maxwell_gmem_size = HEAP_MAX;
    note_mask();
    if (!w->shared_heap) plat_thread_heap_own();
    for (int i; !atomic_load(&w->p->cancel) && (i = atomic_fetch_add(&w->next, 1)) < w->njobs;) {
        const Job *j = &w->jobs[i];
        const SaShader *s = &w->set->shaders[j->shader];
        char why[300] = "";
        uint64_t t0 = SDL_GetTicksNS(), c0 = plat_thread_cpu_ns();
        int r = vk_precompile_one(s->code, s->max_size, s->stage, &j->key, why, sizeof why);
        atomic_fetch_add(&g_st.work_cpu_ns, plat_thread_cpu_ns() - c0);
        atomic_fetch_add(&g_st.work_ns, SDL_GetTicksNS() - t0);
        atomic_fetch_add(&g_st.items, 1);
        note_core();
        int f;
        if (r < 0 && (f = atomic_fetch_add(&w->failed, 1)) < (int)(sizeof w->fails / sizeof w->fails[0])) {
            w->fails[f].shader = j->shader;
            snprintf(w->fails[f].why, sizeof w->fails[f].why, "%s", why);
        } else if (r >= 0) atomic_fetch_add(r > 0 ? &w->made : &w->cached, 1);
        atomic_fetch_add(&w->p->done, 1);
    }
    return NULL;
}

// The failed translations, once, together: by archive, stage and reason, the shaders' ids
static void log_failures(const Work *w) {
    int n = atomic_load(&w->failed), kept = n < (int)(sizeof w->fails / sizeof w->fails[0]) ? n : (int)(sizeof w->fails / sizeof w->fails[0]);
    if (!n) return;
    char out[1900];
    size_t at = (size_t)snprintf(out, sizeof out, "[precompile] %d translations failed, by archive, stage and reason:", n);
    bool done[64] = {false};
    for (int i = 0; i < kept && at < sizeof out; i++) {
        if (done[i]) continue;
        const SaShader *s = &w->set->shaders[w->fails[i].shader];
        at += (size_t)snprintf(out + at, sizeof out - at, "\n  %s, %s: %s:", w->set->sources[s->source],
                               s->stage == STAGE_VERTEX ? "vertex" : s->stage == STAGE_FRAGMENT ? "fragment" : "compute",
                               w->fails[i].why);
        for (int k = i; k < kept && at < sizeof out; k++) {
            const SaShader *t = &w->set->shaders[w->fails[k].shader];
            if (done[k] || t->source != s->source || t->stage != s->stage || strcmp(w->fails[k].why, w->fails[i].why)) continue;
            done[k] = true;
            at += (size_t)snprintf(out + at, sizeof out - at, " %016llx", (unsigned long long)t->hash);
        }
    }
    LOG("%s%s\n", out, kept < n ? "\n  (and more)" : "");
}

// Memory: RADV takes up to ~1.3 GB to compile the largest fragment shaders (41 KB of Maxwell code), and a thread per
// core on the largest at once went past 9.5 GB on a 14 GB laptop (Radeon 660M, 12 threads; at 7 GB the kernel killed
// it). So each compile reserves an estimate from a budget (a quarter of the RAM; SAVAGEWIND_PRECOMPILE_MEM_MB) and a
// thread takes the largest pair left when it fits, else the smallest: the large ones don't end up alone at the end.
// glibc keeps what the driver frees in each thread's arena (7.1 GB at the end of a quarter of the shaders, 2.7 GB
// trimmed after the large ones): malloc_trim after those.
static int64_t pair_need_mb(const Work *w, const Pair *pr) {
    uint32_t bytes = 0;
    int ids[3] = {pr->vs, pr->fs, pr->cs};
    for (int k = 0; k < 3; k++)
        if (ids[k] >= 0 && w->set->shaders[w->jobs[ids[k]].shader].size > bytes) bytes = w->set->shaders[w->jobs[ids[k]].shader].size;
    return 16 + bytes / 32;
}

// The budget. The PS5: the direct memory free now, less 1 GB left to the rest of the title - not a quarter of the
// pool (3 GB of 12): on .41 (precompile-first2.klog) the workers then spent 31-40 s of every 5 s window waiting for
// it at a title heap of only ~1.1 GB, and on the host 13 of the largest compiles at once took ~0.55 GB in all (RADV's
// null NAVI21 device), far under their estimates.
#ifdef __PROSPERO__
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *start_out, size_t *size_out);
#endif
static int64_t budget_mb(void) {
    int64_t quarter = (int64_t)SDL_GetSystemRAM() / 4;
#ifdef __PROSPERO__
    int64_t start = 0;
    size_t free_bytes = 0;
    if (sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &start, &free_bytes) == 0) {
        int64_t mb = (int64_t)(free_bytes >> 20) - 1024;
        LOG("[precompile] direct memory free: %lld MB\n", (long long)(free_bytes >> 20));
        if (mb > quarter) return mb;
    }
#endif
    return quarter;
}

static int pipe_take(Work *w, int64_t *mb) {
    pthread_mutex_lock(&w->lock);
    int i = -1;
    while (w->next_big <= w->next_small && !atomic_load(&w->p->cancel)) {
        int64_t big = w->need_mb[w->order[w->next_big]], small = w->need_mb[w->order[w->next_small]];
        if (!w->used_mb || w->used_mb + big <= w->budget_mb) { i = w->order[w->next_big++]; *mb = big; break; }
        if (w->used_mb + small <= w->budget_mb) { i = w->order[w->next_small--]; *mb = small; break; }
        pthread_cond_wait(&w->cond, &w->lock);
    }
    if (i >= 0) w->used_mb += *mb;
    pthread_mutex_unlock(&w->lock);
    return i;
}

static void pipe_give_back(Work *w, int64_t mb) {
    pthread_mutex_lock(&w->lock);
    w->used_mb -= mb;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

static const int64_t *sort_need;
static int by_need_desc(const void *a, const void *b) {
    int64_t x = sort_need[*(const int *)a], y = sort_need[*(const int *)b];
    return x < y ? 1 : x > y ? -1 : *(const int *)a - *(const int *)b;
}

// The relative cost of a pair's compile, for the bar: the driver's time goes with the code's size (the largest take
// tens of times the smallest)
static uint64_t pair_cost(const Work *w, const Pair *pr) { return 4096 + (uint64_t)(pair_need_mb(w, pr) - 16) * 32; }

static ShaderInfo *timed_read(const SaShader *s, const ShaderKey *key) {
    uint64_t t0 = SDL_GetTicksNS();
    ShaderInfo *si = vk_xlate_read(s->code, s->max_size, s->stage, key);
    atomic_fetch_add(&g_st.read_ns, SDL_GetTicksNS() - t0);
    return si;
}

static void *pipe_worker(void *arg) {
    Work *w = arg;
    int64_t mb;
    plat_thread_heap_own();
    for (int i;;) {
        uint64_t t0 = SDL_GetTicksNS();
        i = pipe_take(w, &mb);
        atomic_fetch_add(&g_st.wait_ns, SDL_GetTicksNS() - t0);
        if (i < 0) break;
        const Pair *pr = &w->pairs[i];
        bool ok = false;
        uint64_t t1, c1;
        if (pr->cs >= 0) {
            const Job *c = &w->jobs[pr->cs];
            const SaShader *s = &w->set->shaders[c->shader];
            ShaderInfo *si = timed_read(s, &c->key);
            t1 = SDL_GetTicksNS(), c1 = plat_thread_cpu_ns();
            if (si) ok = vk_pso_warm_compute(si, s->local);
            maxwell_free(si);
        } else if (pr->vs < 0 || pr->fs < 0) {  // (graphics pipeline libraries: a shader alone)
            const Job *j = &w->jobs[pr->vs >= 0 ? pr->vs : pr->fs];
            const SaShader *sh = &w->set->shaders[j->shader];
            ShaderInfo *si = timed_read(sh, &j->key);
            t1 = SDL_GetTicksNS(), c1 = plat_thread_cpu_ns();
            if (si) ok = vk_pso_warm_stage(si, sh->stage);
            maxwell_free(si);
        } else {
            const Job *v = &w->jobs[pr->vs], *f = &w->jobs[pr->fs];
            const SaShader *vs = &w->set->shaders[v->shader], *fs = &w->set->shaders[f->shader];
            ShaderInfo *vi = timed_read(vs, &v->key);
            ShaderInfo *fi = vi ? timed_read(fs, &f->key) : NULL;
            t1 = SDL_GetTicksNS(), c1 = plat_thread_cpu_ns();
            if (vi && fi) ok = vk_pso_warm(vi, fi, v->key.attr_kind);
            maxwell_free(vi);
            maxwell_free(fi);
        }
        atomic_fetch_add(&g_st.work_cpu_ns, plat_thread_cpu_ns() - c1);
        atomic_fetch_add(&g_st.work_ns, SDL_GetTicksNS() - t1);
        atomic_fetch_add(&g_st.items, 1);
        note_core();
        if (ok) atomic_fetch_add(&w->built, 1);
#ifdef __GLIBC__
        if (mb >= 128) malloc_trim(0);
#endif
        pipe_give_back(w, mb);
        atomic_fetch_add(&w->p->work_done, (long long)pair_cost(w, pr));
        atomic_fetch_add(&w->p->done, 1);
    }
    return NULL;
}

static int pair_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(Pair)); }

static void scan_progress(float f, void *user) {
    ShaderPrecompile *p = user;
    atomic_store(&p->done, (int)(f * 1000));
}

float shader_precompile_progress(const ShaderPrecompile *p) {
    // reading the archives 10%, translating 25%, the pipeline libraries 65% (on the PS5 at 2026-10-28, before the
    // packs: 25 s, 370 s and 480 s); the libraries by their cost (pair_cost), not their count: the smallest take a
    // fraction of a second together
    int st = atomic_load(&p->state), d = atomic_load(&p->done), t = atomic_load(&p->total);
    long long wd = atomic_load(&p->work_done), wt = atomic_load(&p->work_total);
    float f = st == SP_PIPELINES && wt > 0 ? (float)wd / (float)wt : t > 0 ? (float)d / (float)t : 0;
    if (f > 1) f = 1;
    switch (st) {
    case SP_READING: return 0.10f * f;
    case SP_COMPILING: return 0.10f + 0.25f * f;
    case SP_PIPELINES: return 0.35f + 0.65f * f;
    case SP_DONE: return 1;
    default: return 0;
    }
}

// The last window's workers (since the call before): where their time went, and the cores their items finished on
void shader_precompile_log_window(const ShaderPrecompile *p, double seconds, const char *extra) {
    static uint64_t last[5];
    static uint32_t last_core[32];
    static int masks_logged;
    uint64_t now[5] = {atomic_load(&g_st.items), atomic_load(&g_st.work_ns), atomic_load(&g_st.work_cpu_ns),
                       atomic_load(&g_st.read_ns), atomic_load(&g_st.wait_ns)}, d[5];
    for (int i = 0; i < 5; i++) d[i] = now[i] - last[i], last[i] = now[i];
    char cores[300] = "";
    size_t at = 0;
    for (int c = 0; c < 32 && at < sizeof cores; c++) {
        uint32_t n = atomic_load(&g_st.core[c]), dn = n - last_core[c];
        last_core[c] = n;
        if (dn) at += (size_t)snprintf(cores + at, sizeof cores - at, " %d:%u", c, dn);
    }
    int st = atomic_load(&p->state);
    if (st != SP_COMPILING && st != SP_PIPELINES) return;
    LOG("[precompile] last %.0f s on %d threads: %llu done; their work %.1f s (thread CPU in it %.1f s, a clock not trusted "
        "on the PS5), reading %.1f s, waiting for the memory budget %.1f s; items by core:%s%s\n", seconds, p->threads,
        (unsigned long long)d[0], d[1] / 1e9, d[2] / 1e9, d[3] / 1e9, d[4] / 1e9, at ? cores : " (not known)", extra ? extra : "");
    int nm = atomic_load(&g_st.nmask);
    if (!masks_logged && nm > 0) {  // (once: the translation workers' affinity masks, and this thread's)
        masks_logged = 1;
        char out[700];
        size_t o = (size_t)snprintf(out, sizeof out, "[precompile] affinity masks (cpuset bits): this thread %#llx, workers",
                                    (unsigned long long)thread_mask());
        for (int i = 0; i < nm && i < 32 && o < sizeof out; i++) {
            uint64_t m = atomic_load(&g_st.mask[i]);
            o += (size_t)snprintf(out + o, sizeof out - o, m ? " %#llx" : " ?", (unsigned long long)(m - 1));
        }
        LOG("%s\n", out);
    }
}

// The jobs: shader and key, each pair once (a hash set of them: the key log repeats the predicted keys)
typedef struct { Job *jobs; int n, cap; uint64_t *set; size_t set_cap, set_n; } Jobs;

static uint64_t job_hash(int shader, const ShaderKey *k) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)shader;
    const uint8_t *b = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h ? h : 1;
}

static void add_job(Jobs *J, int shader, const ShaderKey *k) {
    uint64_t h = job_hash(shader, k);
    if ((J->set_n + 1) * 2 > J->set_cap) {
        size_t cap = J->set_cap ? J->set_cap * 2 : 1 << 15;
        uint64_t *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < J->set_cap; i++) {
            if (!J->set[i]) continue;
            size_t j = J->set[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = J->set[i];
        }
        free(J->set);
        J->set = t;
        J->set_cap = cap;
    }
    size_t j = h & (J->set_cap - 1);
    for (; J->set[j]; j = (j + 1) & (J->set_cap - 1))
        if (J->set[j] == h) return;  // (that one is there already)
    J->set[j] = h;
    J->set_n++;
    Job **jobs = &J->jobs;
    int *n = &J->n, *cap = &J->cap;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4096;
        *jobs = realloc(*jobs, sizeof **jobs * (size_t)*cap);
    }
    (*jobs)[*n].shader = shader;
    (*jobs)[*n].key = *k;
    (*n)++;
}

static void status_path(char *out, size_t n) { vk_cache_path(out, n, "precompiled.txt"); }

// cache/precompiled.txt: the shader version (vk_shader_version), then "<shaders> <YYYY-MM-DD>"
int shader_precompile_status(int *shaders, char *date, int n) {
    char path[512], version[128] = "", day[32] = "";
    int count = 0;
    status_path(path, sizeof path);
    FILE *f = rt_data_fopen(path, "r");
    if (!f) return 0;
    if (!fgets(version, sizeof version, f) || fscanf(f, "%d %31s", &count, day) != 2) version[0] = 0;
    fclose(f);
    version[strcspn(version, "\r\n")] = 0;
    if (!version[0]) return 0;
    if (shaders) *shaders = count;
    if (date && n > 0) snprintf(date, (size_t)n, "%s", day);
    // (the shaders' libraries go with the GPU and driver: a new driver drops them)
    return strcmp(version, vk_shader_version()) || !vk_precompiled_cache_ok() ? 2 : 1;
}

bool shader_precompile_run(ShaderPrecompile *p, RomfsImage *img, const char *exefs_main) {
    uint64_t t0 = SDL_GetTicksNS();
    atomic_store(&p->done, 0);
    atomic_store(&p->total, 1000);
    atomic_store(&p->state, SP_READING);
    SaSet set;
    if (!shader_archive_scan(img, exefs_main, &set, scan_progress, p, p->why, sizeof p->why)) {
        atomic_store(&p->state, SP_FAILED);
        return false;
    }
    // what the shader key log says: keys by binary, and attribute kinds by archive
    VkKeyLogEntry *log;
    int nlog = vk_keylog_read(&log);
    int (*seen)[32][5] = calloc((size_t)set.nsources, sizeof *seen);
    int origin[2] = {0}, depth[2] = {0};
    for (int i = 0; i < nlog; i++) {
        if (log[i].stage != STAGE_VERTEX) continue;
        origin[log[i].key.flip_y]++;
        depth[log[i].key.depth_minus_one]++;
        int k = shader_archive_find(&set, log[i].code);
        if (k < 0) continue;
        uint32_t in = vk_sph_inputs(set.shaders[k].code, STAGE_VERTEX);
        for (int a = 0; a < 32; a++)
            if ((in >> a) & 1 && log[i].key.attr_kind[a] < 5) seen[set.shaders[k].source][a][log[i].key.attr_kind[a]]++;
    }
    bool flip_y = origin[1] > origin[0], depth_minus_one = depth[1] >= depth[0];  // (BOTW: 0 and 1)
    // the jobs: per shader its predicted key, then the logged ones
    Jobs J = {0};
#ifdef __PROSPERO__
    // Precompilation may run before the renderer starts a frame; use the configured effective mode.
    const bool drs = g_settings.dynamic_resolution && g_settings.antialias != AA_TAA &&
                     g_settings.upscaler != UPSCALE_FSR3;
#endif
    for (int i = 0; i < set.nshaders; i++) {
        const SaShader *s = &set.shaders[i];
        ShaderKey k;
        memset(&k, 0, sizeof k);
        k.glsl = 1;
#ifdef __PROSPERO__
        k.drs = drs;
#endif
        if (s->stage == STAGE_VERTEX) {
            k.flip_y = flip_y;
            k.depth_minus_one = depth_minus_one;
            uint32_t in = vk_sph_inputs(s->code, STAGE_VERTEX);
            const char *src = set.sources[s->source];
            for (int a = 0; a < 32; a++) {
                if (!((in >> a) & 1)) continue;
                int best = -1, most = 0;
                for (int c = 0; c < 5; c++)
                    if (seen[s->source][a][c] > most) most = seen[s->source][a][c], best = c;
                if (best < 0) {
                    best = 0;
                    for (size_t t = 0; t < sizeof kinds / sizeof kinds[0]; t++) {
                        size_t ls = strlen(src), lt = strlen(kinds[t].archive);
                        if (kinds[t].attr == a && ls >= lt && !strcmp(src + ls - lt, kinds[t].archive)) best = kinds[t].kind;
                    }
                }
                k.attr_kind[a] = (uint8_t)best;
            }
        } else if (s->stage == STAGE_FRAGMENT) {
            k.vs_out_mask = vk_sph_inputs(s->code, STAGE_FRAGMENT);
        } else {
            k.shared_size = s->shared;
        }
        vk_key_normalize(s->code, s->stage, &k);
        add_job(&J, i, &k);
    }
    for (int i = 0; i < nlog; i++) {
        int k = shader_archive_find(&set, log[i].code);
        if (k >= 0 && (int)log[i].stage == set.shaders[k].stage) add_job(&J, k, &log[i].key);
    }
    Job *jobs = J.jobs;
    int njobs = J.n;
    free(J.set);
    free(seen);
    free(log);
    LOG("[precompile] %d shaders in %d archives (%llu files, %.0f MB) read in %.1f s: %d translations to make\n", set.nshaders,
        set.nsources, (unsigned long long)set.files, set.bytes / 1048576.0, (SDL_GetTicksNS() - t0) / 1e9, njobs);
    // translate and compile them: into the caches' packs (cache_pack.c: on the PS5 a file costs 11-15 ms to make)
    Work w = {.p = p, .set = &set, .jobs = jobs, .njobs = njobs};
    int nt = SDL_GetNumLogicalCPUCores();
    if (nt < 1) nt = 1;
    if (nt > 32) nt = 32;
    p->threads = nt;
    atomic_store(&p->done, 0);
    atomic_store(&p->total, njobs);
    atomic_store(&p->state, SP_COMPILING);
    pthread_t th[32];
    cache_pack_begin();
    // A scaling check that needs no CPU clock: 40 translations per worker on 1 worker, on 8 and on all of them with
    // heap arenas of their own (plat_thread_heap_own), and on all of them sharing the heap's arenas as other threads
    // do (the same kind of work: archive order). Equal times = the workers really run at once.
    const int batch = 40;
    if (nt > 1 && njobs > batch * (2 * nt + 9) * 4) {
        static const char *const how[4] = {"1 worker", "8 workers, own arenas", "all, own arenas", "all, shared arenas"};
        int counts[4] = {1, nt < 8 ? nt : 8, nt, nt}, start = 0;
        double rate[4];
        char out[400];
        size_t at = (size_t)snprintf(out, sizeof out, "[precompile] scaling (translations per second):");
        for (int k = 0; k < 4; k++) {
            uint64_t s0 = SDL_GetTicksNS();
            w.shared_heap = k == 3;
            atomic_store(&w.next, start);
            w.njobs = start + batch * counts[k];
            for (int i = 0; i < counts[k]; i++) pthread_create(&th[i], NULL, worker, &w);
            for (int i = 0; i < counts[k]; i++) pthread_join(th[i], NULL);
            rate[k] = batch * counts[k] / ((SDL_GetTicksNS() - s0) / 1e9);
            start += batch * counts[k];
            at += (size_t)snprintf(out + at, sizeof out - at, " %s %.0f (%.1fx);", how[k], rate[k], rate[k] / rate[0]);
        }
        LOG("%s\n", out);
        w.shared_heap = false;
        w.njobs = njobs;  // (each worker's last take went past its batch's end: none skipped)
        atomic_store(&w.next, start);
    }
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, &w);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    cache_pack_end();
    vk_keylog_flush();
    LOG("[precompile] %d translations in %.1f s on %d threads: %d made, %d were cached, %d failed\n", njobs,
        (SDL_GetTicksNS() - t0) / 1e9, nt, atomic_load(&w.made), atomic_load(&w.cached), atomic_load(&w.failed));
    log_failures(&w);
    // into the pipeline cache: the compute pipelines (their whole state is known: the group size); with graphics
    // pipeline libraries (exec.c), every vertex and fragment shader's libraries, into a cache of their own
    // (cache/vk_precompiled.bin, device.c). Without them, whole pipelines only
    // with SAVAGEWIND_PRECOMPILE_PIPELINES=1, one per program: those take ~0.85 GB of pipeline cache and help less
    // (in a walk where the driver had seen nothing: 12.7 -> 8.4 s of compiles on the render thread), since a later
    // draw with other blending, target formats or write masks costs the driver much of a compile again.
    bool graphics = !g_vk.gpl && getenv("SAVAGEWIND_PRECOMPILE_PIPELINES") != NULL;
    if (g_vk.dev && !atomic_load(&p->cancel) && !getenv("SAVAGEWIND_PRECOMPILE_NO_PIPELINES")) {
        int *first = malloc(sizeof *first * (size_t)(set.nshaders ? set.nshaders : 1));  // shader -> its predicted job
        for (int i = 0; i < set.nshaders; i++) first[i] = -1;
        for (int i = 0; i < njobs; i++)
            if (first[jobs[i].shader] < 0) first[jobs[i].shader] = i;
        Pair *pairs = malloc(sizeof *pairs * (size_t)(set.nprograms ? set.nprograms : 1));
        int np = 0;
        for (int i = 0; i < set.nprograms; i++) {
            const SaProgram *g = &set.programs[i];
            Pair pr = {-1, -1, -1};
            if (g->cs >= 0) pr.cs = first[g->cs];
            else pr.vs = first[g->vs], pr.fs = first[g->fs];
            if ((g->cs >= 0 && pr.cs >= 0) || (graphics && g->cs < 0 && pr.vs >= 0 && pr.fs >= 0)) pairs[np++] = pr;
        }
        if (g_vk.gpl) {
            pairs = realloc(pairs, sizeof *pairs * (size_t)(np + set.nshaders + 1));
            for (int i = 0; i < set.nshaders; i++) {
                if (first[i] < 0 || set.shaders[i].stage == STAGE_COMPUTE) continue;
                pairs[np++] = set.shaders[i].stage == STAGE_VERTEX ? (Pair){first[i], -1, -1} : (Pair){-1, first[i], -1};
            }
        }
        qsort(pairs, (size_t)np, sizeof *pairs, pair_cmp);
        int nu = 0;
        for (int i = 0; i < np; i++)
            if (!nu || memcmp(&pairs[i], &pairs[nu - 1], sizeof *pairs)) pairs[nu++] = pairs[i];
        w.pairs = pairs;
        w.npairs = nu;
        w.need_mb = malloc(sizeof *w.need_mb * (size_t)(nu ? nu : 1));
        w.order = malloc(sizeof *w.order * (size_t)(nu ? nu : 1));
        for (int i = 0; i < nu; i++) w.need_mb[i] = pair_need_mb(&w, &pairs[i]), w.order[i] = i;
        sort_need = w.need_mb;
        qsort(w.order, (size_t)nu, sizeof *w.order, by_need_desc);
        w.next_big = 0;
        w.next_small = nu - 1;
        w.used_mb = 0;
        const char *mem = getenv("SAVAGEWIND_PRECOMPILE_MEM_MB");
        w.budget_mb = mem && atoll(mem) > 0 ? atoll(mem) : budget_mb();
        LOG("[precompile] memory budget for the driver's compiles: %lld MB (estimates: 16 MB + 1 MB per 32 bytes of code)\n",
            (long long)w.budget_mb);
        pthread_mutex_init(&w.lock, NULL);
        pthread_cond_init(&w.cond, NULL);
        atomic_store(&w.next, 0);
        long long cost = 0;
        for (int i = 0; i < nu; i++) cost += (long long)pair_cost(&w, &pairs[i]);
        atomic_store(&p->work_done, 0);
        atomic_store(&p->work_total, cost);
        atomic_store(&p->done, 0);
        atomic_store(&p->total, nu);
        atomic_store(&p->state, SP_PIPELINES);
        uint64_t tp = SDL_GetTicksNS();
        for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, pipe_worker, &w);
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
        p->pipelines = atomic_load(&w.built);
        extern void vk_pipeline_cache_save(void);
        vk_pipeline_cache_save();
        if (g_vk.gpl) vk_precompiled_cache_save();
        LOG("[precompile] %d %s: %d built in %.1f s\n", nu, g_vk.gpl ? "shaders and compute programs (pipeline libraries)" : "programs",
            p->pipelines, (SDL_GetTicksNS() - tp) / 1e9);
        free(pairs);
        free(first);
        free(w.order);
        free(w.need_mb);
        pthread_mutex_destroy(&w.lock);
        pthread_cond_destroy(&w.cond);
    }
    p->shaders = set.nshaders;
    p->keys = njobs;
    p->made = atomic_load(&w.made);
    p->cached = atomic_load(&w.cached);
    p->failed = atomic_load(&w.failed);
    free(jobs);
    shader_archive_free(&set);
    if (atomic_load(&p->cancel)) {
        atomic_store(&p->state, SP_CANCELLED);
        return false;
    }
    p->seconds = (SDL_GetTicksNS() - t0) / 1e9;
    char path[512];
    status_path(path, sizeof path);
#ifdef __PROSPERO__
    time_t now = time(NULL);
    char date[32], content[160];
    strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    int n = snprintf(content, sizeof content, "%s\n%d %s\n", vk_shader_version(), p->shaders, date);
    if (n >= 0 && (size_t)n < sizeof content) rt_storage_replace(path, content, (size_t)n);
#else
    FILE *f = fopen(path, "w");
    if (f) {
        time_t now = time(NULL);
        char date[32];
        strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
        fprintf(f, "%s\n%d %s\n", vk_shader_version(), p->shaders, date);
        fclose(f);
    }
#endif
    atomic_store(&p->state, SP_DONE);
    return true;
}
