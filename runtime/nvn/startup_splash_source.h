// Ordered startup image selection shared with the host behavioral test.
#ifndef STARTUP_SPLASH_SOURCE_H
#define STARTUP_SPLASH_SOURCE_H
#include <stddef.h>

typedef unsigned char *(*StartupSplashLoad)(const char *path, int *width, int *height, void *context);
static inline unsigned char *startup_splash_choose(const char *user, const char *cached, const char *shipped,
                                                    StartupSplashLoad load, void *context,
                                                    int *width, int *height, const char **selected) {
    const char *paths[] = {user, cached, shipped};
    if (selected) *selected = NULL;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        if (!paths[i] || !paths[i][0]) continue;
        unsigned char *image = load(paths[i], width, height, context);
        if (image) {
            if (selected) *selected = paths[i];
            return image;
        }
    }
    return NULL;
}
#endif
