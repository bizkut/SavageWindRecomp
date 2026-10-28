// PS5_MihawkGen's neural frame generation behind Savage Wind's frame-generation seam (framegen.h; docs/PS5-PORT-PLAN.md
// D7): the PS5 build's backend. exec.c fg_mihawk owns every image and barrier (the HUD-less images of the last two
// game frames, the output) and asks here for the frame halfway between them; MihawkGen's interface (framegen/include/
// framegen.h) records its compute passes into that command buffer. Only its API, contract and GPU-timestamp method
// (fg_context_set_profile) are used, not its WoW/vkd3d bridge. The weights are network.fgw in the title folder; missing
// or invalid weights turn frame generation off with the reason logged.
//
// GPL-3.0-or-later like MihawkGen's framegen/ (D10): linked into the PS5 title only (ps5/tools/build-game.sh, at the
// revision pinned in ps5/deps.lock); the Makefile leaves this file out of the desktop build.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "framegen.h"                   // Savage Wind's seam
#include "framegen/include/framegen.h"  // MihawkGen's (-I of the pinned checkout)

#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "../../paths.h"
void ps5_logf(const char *format, ...);  // (runtime/ps5/log.c: klog)
}
#define LOG(...) ps5_logf(__VA_ARGS__)

namespace {

enum { MAX_SLOTS = 8, MAX_QUERIES = 1024 };

struct {
    fg_context *ctx;
    uint32_t w, h, slots;
    VkQueryPool pool;       // the neural passes' timestamps: (passes + 1) per slot
    uint32_t passes;
    double tick_ns;         // timestampPeriod
    bool timed[MAX_SLOTS];  // the slot's last recording wrote timestamps not read yet
} m;

void release() {
    if (m.pool) vkDestroyQueryPool(g_fg_vk.dev, m.pool, nullptr);
    fg_context_destroy(m.ctx);
    m = {};
}

// network.fgw from the title folder; false: missing or unreadable (logged)
bool read_weights(void **blob, size_t *size, char *path, size_t n) {
    if (!rt_resource("network.fgw", nullptr, path, n)) {
        LOG("[fg] MihawkGen off: network.fgw is not in the title folder\n");
        return false;
    }
    FILE *f = fopen(path, "rb");
    long len = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    void *p = len > 0 ? malloc((size_t)len) : nullptr;
    bool ok = p && fseek(f, 0, SEEK_SET) == 0 && fread(p, 1, (size_t)len, f) == (size_t)len;
    if (f) fclose(f);
    if (!ok) {
        free(p);
        LOG("[fg] MihawkGen off: %s could not be read\n", path);
        return false;
    }
    *blob = p, *size = (size_t)len;
    return true;
}

bool ready(uint32_t w, uint32_t h, uint32_t slots) {
    if (m.ctx && m.w == w && m.h == h && m.slots == slots) return true;
    if (m.ctx) {
        g_fg_vk.wait_idle();
        release();
    }
    if (slots > MAX_SLOTS) slots = MAX_SLOTS;
    fg_create_info ci = {};
    ci.device = g_fg_vk.dev;
    ci.physical_device = g_fg_vk.phys;
    ci.width = w, ci.height = h, ci.slots = slots;
    ci.output_format = VK_FORMAT_R8G8B8A8_UNORM;
    char err[512] = "";
    if (fg_context_create(&ci, &m.ctx, err, sizeof err) != VK_SUCCESS) {
        LOG("[fg] MihawkGen off: %s\n", err);
        m = {};
        return false;
    }
    char path[512];
    void *blob = nullptr;
    size_t size = 0;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (read_weights(&blob, &size, path, sizeof path)) {
        r = fg_context_load_network(m.ctx, blob, size, err, sizeof err);  // (the context keeps a copy)
        free(blob);
        if (r != VK_SUCCESS) LOG("[fg] MihawkGen off: the weights in %s: %s\n", path, err);
    }
    if (r != VK_SUCCESS || !fg_method_available(m.ctx, FG_METHOD_NEURAL)) {
        release();
        return false;
    }
    m.w = w, m.h = h, m.slots = slots;
    // GPU time (MihawkGen's method: a timestamp before the first pass and after each)
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_fg_vk.phys, &props);
    m.passes = fg_neural_pass_count(m.ctx);
    m.tick_ns = props.limits.timestampPeriod;
    if (props.limits.timestampComputeAndGraphics && (m.passes + 1) * slots <= MAX_QUERIES) {
        VkQueryPoolCreateInfo qi = {};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = (m.passes + 1) * slots;
        if (vkCreateQueryPool(g_fg_vk.dev, &qi, nullptr, &m.pool) != VK_SUCCESS) m.pool = VK_NULL_HANDLE;
    }
    LOG("[fg] MihawkGen neural %ux%u, %u slots, %u passes, weights %s (%zu bytes); GPU time %s\n", w, h, slots, m.passes,
        path, size, m.pool ? "per frame (timestamps)" : "not measured (no timestamps)");
    return true;
}

void record(VkCommandBuffer cb, uint32_t slot, VkImageView prev, VkImageView next, VkImageView out) {
    if (!m.ctx || slot >= m.slots) return;
    if (m.pool) {
        uint32_t base = slot * (m.passes + 1);
        vkCmdResetQueryPool(cb, m.pool, base, m.passes + 1);
        fg_context_set_profile(m.ctx, m.pool, base);
    }
    fg_frame_pair pair = {};
    pair.prev_color = prev;
    pair.next_color = next;
    pair.t = 0.5f;
    fg_record_generate(m.ctx, cb, slot, FG_METHOD_NEURAL, &pair, out);
    m.timed[slot] = m.pool != VK_NULL_HANDLE;
}

bool gpu_ns(uint32_t slot, uint64_t *ns) {
    if (slot >= m.slots || !m.timed[slot]) return false;
    m.timed[slot] = false;
    static uint64_t t[MAX_QUERIES];  // (one slot's passes + 1 at most)
    if (vkGetQueryPoolResults(g_fg_vk.dev, m.pool, slot * (m.passes + 1), m.passes + 1, sizeof t, t, sizeof t[0],
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || t[m.passes] < t[0])
        return false;
    *ns = (uint64_t)((double)(t[m.passes] - t[0]) * m.tick_ns);
    return true;
}

}  // namespace

// The PS5 build's frame generation (framegen.h; desktop's is framegen.cpp)
extern "C" const FgBackend g_fg_backend = {FG_BACKEND_MIHAWK, "PS5_MihawkGen neural", nullptr, ready, record, gpu_ns};
