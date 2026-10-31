#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Called before a picture finalizes: only restore slots still holding the value we installed.
static size_t action_prompt_restore_slots(uint64_t *const *slots, const uint64_t *original,
                                          const uint64_t *expected, size_t count) {
    size_t restored = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t current;
        memcpy(&current, slots[i], sizeof current);
        if (current != expected[i]) continue;
        memcpy(slots[i], &original[i], sizeof current);
        restored++;
    }
    return restored;
}
