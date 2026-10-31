// cc tools/test_fd_census.c -o /tmp/test_fd_census && /tmp/test_fd_census
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../runtime/platform/fd_census.h"

int main(void) {
    struct fd_census before = fd_census_scan(1024);
    char path[] = "/tmp/savagewind-census-XXXXXX";
    int file = mkstemp(path);
    int directory = open("/tmp", O_RDONLY | O_DIRECTORY);
    int sockets[2];
    int pipefd[2];
    assert(file >= 0 && directory >= 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(pipe(pipefd) == 0);
    assert(file < 1024 && directory < 1024 && sockets[0] < 1024 && sockets[1] < 1024 &&
           pipefd[0] < 1024 && pipefd[1] < 1024);
    struct fd_census during = fd_census_scan(1024);
    assert(during.regular == before.regular + 1);
    assert(during.directories == before.directories + 1);
    assert(during.sockets == before.sockets + 2);
    assert(during.other == before.other + 2);
    assert(close(file) == 0 && close(directory) == 0);
    assert(close(sockets[0]) == 0 && close(sockets[1]) == 0);
    assert(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
    assert(unlink(path) == 0);
    struct fd_census after = fd_census_scan(1024);
    assert(after.regular == before.regular && after.directories == before.directories &&
           after.sockets == before.sockets && after.other == before.other);
    return 0;
}
