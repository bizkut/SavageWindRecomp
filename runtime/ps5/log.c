// Synchronous klog output for runtime LOG and FATAL messages.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int sceKernelDebugOutText(int channel, const char *text);
void ps5_logf(const char *format, ...) {
    char line[2048] = "savagewind: ";
    size_t prefix = strlen(line);
    va_list args;
    va_start(args, format);
    vsnprintf(line + prefix, sizeof line - prefix, format, args);
    va_end(args);
    sceKernelDebugOutText(0, line);
}
