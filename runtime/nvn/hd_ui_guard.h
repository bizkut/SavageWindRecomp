// A guest non-sampling use permanently blocks HD replacement for the current texture body.
#pragma once
#include <stdbool.h>
static inline bool hd_ui_begin_guest_use(bool *blocked, bool replacement_active) {
    bool restore = !*blocked && replacement_active;
    *blocked = true;
    return restore;
}
