#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GAMECODE_GENERATOR_SHA256 ""
#define GAMECODE_COMPILE_SHA256 ""
#define GAMECODE_LLVM_SHA256 ""
#include "../runtime/platform/gamecode_ps5.c"

static uint64_t now_ns;
static char saved[1024];
static unsigned writes;
static int broker_failure;

uint64_t plat_now_ns(void) {
    return now_ns;
}

int rt_storage_replace(const char *path, const void *data, size_t size) {
    assert(!strcmp(path, "cache/gamecode/last-build.txt"));
    assert(size < sizeof saved);
    memcpy(saved, data, size);
    saved[size] = 0;
    writes++;
    return broker_failure ? -1 : 0;
}

int rt_storage_mkdirs(const char *path) {
    assert(!strcmp(path, "cache/gamecode"));
    return 0;
}

void ps5_heap_stats(struct ps5_heap_stats *stats) {
    memset(stats, 0, sizeof *stats);
    stats->peak_bytes = 4321;
}

static void expect(const char *line) {
    assert(strstr(saved, line));
}

int main(void) {
    struct build_receipt receipt = {.start = 1000000000, .stage_start = 1000000000,
                                    .stage = RECEIPT_IDENTITY, .threads = 4, .overlap = 1};
    struct progress_state progress = {0};
    struct object_stats stats = {.hits = 7, .misses = 3};
    now_ns = 3000000000;
    receipt_stage(&receipt, RECEIPT_CACHE_LOAD);
    now_ns = 4000000000;
    receipt_stage(&receipt, RECEIPT_GENERATE_COMPILE);
    now_ns = 4500000000;
    report_progress(&progress, "generate", 0, 3);
    now_ns = 5000000000;
    report_progress(&progress, "compile-start", 0, 0);
    now_ns = 6000000000;
    report_progress(&progress, "compile-start", 0, 0);
    now_ns = 7000000000;
    report_progress(&progress, "compile-finish", 0, 0);
    now_ns = 7500000000;
    report_progress(&progress, "generate", 3, 3);
    now_ns = 8000000000;
    report_progress(&progress, "compile-finish", 0, 0);
    now_ns = 9000000000;
    receipt_stage(&receipt, RECEIPT_LINK);
    now_ns = 11000000000;
    receipt_stage(&receipt, RECEIPT_SAVE);
    now_ns = 12000000000;
    receipt_stage(&receipt, RECEIPT_CACHE_RELOAD);
    now_ns = 13000000000;
    receipt_stage(&receipt, RECEIPT_VALIDATE);
    now_ns = 14000000000;
    receipt_stage(&receipt, receipt.stage);
    write_receipt(&receipt, &progress, &stats, 12345, 1);
    assert(writes == 1);
    expect("status=ready\nthreads=4\noverlap=1\nimage_cache_hit=0\n");
    expect("object_cache_hits=7\nobject_cache_misses=3\n");
    expect("identity_seconds=2.000\ncache_load_seconds=1.000\n");
    expect("generation_seconds=3.000\ncompile_seconds=3.000\ngeneration_compile_seconds=5.000\n");
    expect("link_seconds=2.000\nsave_seconds=1.000\ncache_reload_seconds=1.000\nvalidate_seconds=1.000\n");
    expect("peak_heap_bytes=12345\ntotal_seconds=13.000\n");

    struct build_receipt failure = {.start = 20000000000, .stage_start = 20000000000,
                                    .stage = RECEIPT_IDENTITY};
    struct progress_state no_progress = {0};
    struct object_stats no_stats = {0};
    now_ns = 22500000000;
    receipt_stage(&failure, failure.stage);
    write_receipt(&failure, &no_progress, &no_stats, 99, 0);
    assert(writes == 2);
    expect("status=failed\nthreads=0\noverlap=0\nimage_cache_hit=0\n");
    expect("identity_seconds=2.500\ncache_load_seconds=0.000\n");
    expect("generation_seconds=0.000\ncompile_seconds=0.000\ngeneration_compile_seconds=0.000\n");
    expect("peak_heap_bytes=99\ntotal_seconds=2.500\n");
    struct progress_state interrupted = {0};
    now_ns = 23000000000;
    report_progress(&interrupted, "compile-start", 0, 0);
    now_ns = 24500000000;
    finish_compiles(&interrupted);
    assert(interrupted.compile_elapsed == 1500000000 && !interrupted.compile_active);
    broker_failure = 1;
    write_receipt(&failure, &no_progress, &no_stats, 99, 0);
    assert(writes == 3);
    broker_failure = 0;
    ps5_gamecode_record_start_failure(99, true);
    assert(writes == 4);
    expect("status=failed\nthreads=0\noverlap=1\nimage_cache_hit=0\nrequested_threads=16\n");
    expect("peak_heap_bytes=4321\n");
    puts("gamecode receipt tests passed");
    return 0;
}
