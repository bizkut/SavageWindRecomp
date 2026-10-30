// First-start home-art receipt. No installed files are read on the fast path.
#ifndef HOME_ART_GATE_H
#define HOME_ART_GATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HOME_ART_MARKER_FORMAT 1u
#define HOME_ART_GENERATOR_VERSION 3u

typedef struct {
    unsigned char magic[8];
    uint32_t format;
    uint32_t generator;
    uint32_t mode;
    unsigned char source[128];
    // For disabled mode, only installed[0] is an icon digest; background slots are zero.
    unsigned char installed[3][32];
    uint32_t checksum;
} HomeArtMarker;

static uint32_t home_art_marker_checksum(const HomeArtMarker *marker) {
    const unsigned char *bytes = (const unsigned char *)marker;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(HomeArtMarker, checksum); i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static int home_art_marker_matches(const HomeArtMarker *marker, size_t size, int enabled,
                                   const unsigned char source[128]) {
    return size == sizeof *marker && !memcmp(marker->magic, "SWART01", 8) &&
        marker->format == HOME_ART_MARKER_FORMAT && marker->generator == HOME_ART_GENERATOR_VERSION &&
        marker->mode == (unsigned)(enabled != 0) && !memcmp(marker->source, source, 128) &&
        marker->checksum == home_art_marker_checksum(marker);
}

static void home_art_marker_make(HomeArtMarker *marker, int enabled, const unsigned char source[128],
                                 const unsigned char installed[3][32]) {
    memset(marker, 0, sizeof *marker);
    memcpy(marker->magic, "SWART01", 8);
    marker->format = HOME_ART_MARKER_FORMAT;
    marker->generator = HOME_ART_GENERATOR_VERSION;
    marker->mode = enabled != 0;
    memcpy(marker->source, source, 128);
    memcpy(marker->installed, installed, sizeof marker->installed);
    marker->checksum = home_art_marker_checksum(marker);
}

typedef struct {
    size_t (*read)(void *, HomeArtMarker *);
    int (*remove)(void *);
    int (*install)(void *, int, unsigned char [3][32]);
    int (*write)(void *, const HomeArtMarker *);
} HomeArtGateOps;

// 0 = current, 1 = installed, negative = failure. Invalidate before changing any installed file.
static int home_art_gate_run(const HomeArtGateOps *ops, void *context, int enabled,
                             const unsigned char source[128]) {
    HomeArtMarker marker;
    unsigned char installed[3][32];
    size_t size = ops->read(context, &marker);
    if (home_art_marker_matches(&marker, size, enabled, source)) return 0;
    if (ops->remove(context) < 0) return -1;
    if (ops->install(context, enabled, installed) < 0) return -2;
    home_art_marker_make(&marker, enabled, source, installed);
    if (ops->write(context, &marker) < 0) return -3;
    return 1;
}
#endif
