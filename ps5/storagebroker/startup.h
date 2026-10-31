/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SAVAGE_STORAGE_STARTUP_H
#define SAVAGE_STORAGE_STARTUP_H
#include "client.h"

/* Returns 0 with a connected client, or a negative errno. loader_refused is set
 * only when the loopback ELF loader refused the connection. */
int sb_startup(struct sb_client *client, int *loader_refused);

struct sb_startup_ops {
    int (*connect_broker)(struct sb_client *client);
    void (*close_broker)(struct sb_client *client);
    int (*launch)(void);
    void (*pause)(void);
};
int sb_startup_with_ops(struct sb_client *client, int *loader_refused, const struct sb_startup_ops *ops);
#endif
