/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "startup.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

static int initial_version, launch_result, attempts, launches, closes, pauses;
static int connect_fake(struct sb_client *client)
{
    attempts++;
    if (attempts == 1 && !initial_version) return -ECONNREFUSED;
    client->fd = 1;
    client->server_version = attempts == 1 ? (unsigned)initial_version : SB_VERSION;
    return 0;
}
static void close_fake(struct sb_client *client)
{
    closes++;
    client->fd = -1;
    client->server_version = 0;
}
static int launch_fake(void)
{
    launches++;
    return launch_result;
}
static void pause_fake(void) { pauses++; }
int sb_connect(struct sb_client *client) { (void)client; return -1; }
void sb_close(struct sb_client *client) { (void)client; }

static void check(int version, int loader, int expected, int expected_launches, int refused)
{
    const struct sb_startup_ops ops = {connect_fake, close_fake, launch_fake, pause_fake};
    struct sb_client client = {-1, 0};
    int loader_refused = -1;
    initial_version = version;
    launch_result = loader;
    attempts = launches = closes = pauses = 0;
    assert(sb_startup_with_ops(&client, &loader_refused, &ops) == expected);
    assert(launches == expected_launches);
    assert(loader_refused == refused);
    assert(attempts == 1 + (expected_launches && !loader));
}
int main(void)
{
    check(SB_VERSION, 0, 0, 0, 0);
    check(SB_VERSION - 1, 0, 0, 1, 0);
    check(SB_VERSION + 1, 0, 0, 0, 0);
    check(0, 0, 0, 1, 0);
    check(0, -ECONNREFUSED, -ECONNREFUSED, 1, 1);
    puts("broker startup decisions passed");
    return 0;
}
