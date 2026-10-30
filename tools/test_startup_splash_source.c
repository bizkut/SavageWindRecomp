// Host test for user, generated and shipped startup-image precedence.
#include "../runtime/nvn/startup_splash_source.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture { const char *available; int calls; };
static unsigned char *load(const char *path, int *width, int *height, void *context) {
    struct fixture *f = context;
    f->calls++;
    if (!strstr(f->available, path)) return NULL;
    *width = 3840;
    *height = 2160;
    return (unsigned char *)f;
}

int main(void) {
    const char *selected;
    int width = 0, height = 0;
    struct fixture f = {"user cache shipped", 0};
    assert(startup_splash_choose("user", "cache", "shipped", load, &f, &width, &height, &selected));
    assert(!strcmp(selected, "user") && f.calls == 1);
    f = (struct fixture){"cache shipped", 0};
    assert(startup_splash_choose("user", "cache", "shipped", load, &f, &width, &height, &selected));
    assert(!strcmp(selected, "cache") && f.calls == 2);
    f = (struct fixture){"shipped", 0};
    assert(startup_splash_choose("", "cache", "shipped", load, &f, &width, &height, &selected));
    assert(!strcmp(selected, "shipped") && f.calls == 2);
    f = (struct fixture){"", 0};
    assert(!startup_splash_choose("user", "cache", "shipped", load, &f, &width, &height, &selected));
    assert(!selected && f.calls == 3);
    puts("startup splash source precedence: ok");
    return 0;
}
