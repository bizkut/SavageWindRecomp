// Count open descriptors with fstat, as used by the PS5 title descriptor census.
#pragma once
#include <sys/stat.h>

struct fd_census {
    unsigned regular, directories, sockets, other;
};

static inline struct fd_census fd_census_scan(int end) {
    struct fd_census count = {0};
    for (int fd = 0; fd < end; fd++) {
        struct stat st;
        if (fstat(fd, &st)) continue;
        if (S_ISREG(st.st_mode)) count.regular++;
        else if (S_ISDIR(st.st_mode)) count.directories++;
        else if (S_ISSOCK(st.st_mode)) count.sockets++;
        else count.other++;
    }
    return count;
}
