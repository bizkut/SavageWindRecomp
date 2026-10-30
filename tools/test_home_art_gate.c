// Host contract for the first-start home-art receipt.
#include "../runtime/platform/home_art_gate.h"
#include <assert.h>
#include <stdio.h>

typedef struct {
    HomeArtMarker marker;
    size_t size;
    int installs;
    int writes;
    int fail_install;
} Fixture;

static size_t read_marker(void *ctx, HomeArtMarker *marker) {
    Fixture *f = ctx;
    if (f->size) *marker = f->marker;
    return f->size;
}

static int remove_marker(void *ctx) {
    Fixture *f = ctx;
    f->size = 0;
    return 0;
}

static int install(void *ctx, int enabled, unsigned char digests[3][32]) {
    Fixture *f = ctx;
    f->installs++;
    if (f->fail_install) return -1;
    memset(digests, enabled ? 1 : 2, 96);
    return 0;
}

static int write_marker(void *ctx, const HomeArtMarker *marker) {
    Fixture *f = ctx;
    f->marker = *marker;
    f->size = sizeof *marker;
    f->writes++;
    return 0;
}

int main(void) {
    const HomeArtGateOps ops = {read_marker, remove_marker, install, write_marker};
    unsigned char source[128] = {1};
    Fixture f = {0};
    assert(home_art_gate_run(&ops, &f, 1, source) == 1);
    assert(f.installs == 1 && f.writes == 1);
    assert(home_art_gate_run(&ops, &f, 1, source) == 0);
    assert(f.installs == 1 && f.writes == 1);
    source[3] = 1;
    assert(home_art_gate_run(&ops, &f, 1, source) == 1);
    assert(home_art_gate_run(&ops, &f, 0, source) == 1);
    f.marker.installed[0][0] ^= 1;
    assert(home_art_gate_run(&ops, &f, 0, source) == 1);
    source[4] = 1;
    f.fail_install = 1;
    assert(home_art_gate_run(&ops, &f, 0, source) == -2);
    assert(f.size == 0 && f.writes == 4);
    puts("home-art gate: ok");
    return 0;
}
