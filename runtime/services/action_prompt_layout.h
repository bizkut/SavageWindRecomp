// Reposition the action diamond using the actual face-button bindings in a validated FLYT.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static float action_prompt_float(const uint8_t *p) {
    float value;
    memcpy(&value, p, sizeof value);
    return value;
}
static void action_prompt_float_set(uint8_t *p, float value) { memcpy(p, &value, sizeof value); }

// positions are SDL's SOUTH/EAST/WEST/NORTH values; a non-face or duplicate binding has no diamond position.
static bool action_prompt_layout(uint8_t *b, size_t n, int active, const int positions[4]) {
    static const int original[] = {1, 0, 3, 2};
    static const float icon_xy[4][2] = {{1, -26}, {26.25f, 0.75f}, {-25.5f, -0.25f}, {0.75f, 26}};
    static const float active_xy[4][2] = {{0, -26}, {26, 0}, {-26, 0}, {0, 26}};
    static const char *const active_names[] = {"N_BtnA_00", "N_BtnB_00", "N_BtnX_00", "N_BtnX_00"};
    static const char *const capture_names[] = {"N_CaptureA_00", "N_CaptureB_00", "N_CaptureX_00", "N_CaptureY_00"};
    static const char *const base_names[] = {"P_Base_00", "P_BaseB_00", "P_BaseX_00", "P_BaseY_00"};
    static const char *const letters[4][4] = {
        {NULL, "T_B_00", "T_X_00", "T_Y_00"},
        {"T_A_01", NULL, "T_X_00", "T_Y_00"},
        {"T_A_00", "T_B_00", NULL, "T_Y_00"},
        {"T_A_00", "T_B_00", "T_X_00", NULL},
    };
    static const char *const guide0[] = {"T_GuideA_00", "T_GuideB_00", "T_GuideX_00", "T_GuideY_00"};
    static const char *const guide1[] = {"T_GuideA_01", "T_GuideB_01", "T_GuideX_01", "T_GuideY_01"};
    static const char *const icon1[] = {"Nt_KeyTexA_01", "Nt_KeyTexB_01", "Nt_KeyTexX_01", "Nt_KeyTexY_01"};
    static const char *const windows[] = {"W_BaseA_00", "W_BaseB_00", "W_BaseX_00", "W_BaseY_00"};
    static const char base_a[] = "Nt_KeyBtnSubBaseAL_00^s";
    static const char base_b[] = "Nt_KeyBtnSubBaseBL_00^s";
    if (active < 0 || active > 3 || n < 0x14 || memcmp(b, "FLYT", 4) || b[4] != 0xff || b[5] != 0xfe)
        return false;
    unsigned seen = 0;
    for (int i = 0; i < 4; i++) {
        if (positions[i] < 0 || positions[i] > 3 || (seen & (1u << positions[i]))) return false;
        seen |= 1u << positions[i];
    }
    const char *names[15] = {active_names[active], capture_names[active], base_names[active],
                             letters[active][0], letters[active][1], letters[active][2], letters[active][3],
                             guide0[active], guide1[active], icon1[active], windows[active],
                             active == 0 ? "P_CaptureUse00_00" : "P_CaptureUse_02",
                             active == 3 ? "P_CaptureUse00_03" : NULL,
                             active == 3 ? "P_CaptureUse00_04" : NULL,
                             active == 3 ? "P_CaptureUse00_05" : NULL};
    uint8_t *pane[15] = {0};
    uint8_t *base = NULL;
    size_t pos = b[6] | (size_t)b[7] << 8;
    while (pos + 8 <= n) {
        uint32_t size;
        memcpy(&size, b + pos + 4, 4);
        if (size < 8 || size > n - pos) return false;
        if (!memcmp(b + pos, "txl1", 4)) {
            for (size_t i = 8; i + sizeof base_a <= size; i++) {
                if (!memcmp(b + pos + i, base_a, sizeof base_a) || !memcmp(b + pos + i, base_b, sizeof base_b)) {
                    if (base) return false;
                    base = b + pos + i;
                }
            }
        }
        if (size >= 0x60 && (!memcmp(b + pos, "pan1", 4) || !memcmp(b + pos, "pic1", 4) ||
                             !memcmp(b + pos, "txt1", 4) || !memcmp(b + pos, "wnd1", 4))) {
            for (int i = 0; i < 15; i++) {
                if (!names[i]) continue;
                size_t len = strlen(names[i]);
                if (!memcmp(b + pos + 12, names[i], len + 1)) {
                    if (pane[i]) return false;
                    pane[i] = b + pos;
                }
            }
        }
        // pan1 sections are shorter than pictures and text, but still contain the transform.
        if (size >= 0x54 && size < 0x60 && !memcmp(b + pos, "pan1", 4)) {
            for (int i = 0; i < 2; i++) {
                size_t len = strlen(names[i]);
                if (!memcmp(b + pos + 12, names[i], len + 1)) {
                    if (pane[i]) return false;
                    pane[i] = b + pos;
                }
            }
        }
        pos += size;
    }
    if (pos != n || !base) return false;
    for (int i = 0; i < 15; i++) if (names[i] && !pane[i]) return false;
    int src = original[active], dst = positions[active];
    if (action_prompt_float(pane[0] + 0x2c) != active_xy[src][0] ||
        action_prompt_float(pane[0] + 0x30) != active_xy[src][1]) return false;
    for (int i = 0; i < 4; i++) {
        if (i == active) continue;
        if (action_prompt_float(pane[3 + i] + 0x2c) != icon_xy[original[i]][0] ||
            action_prompt_float(pane[3 + i] + 0x30) != icon_xy[original[i]][1]) return false;
    }
    float capture_y = action_prompt_float(pane[1] + 0x30) - active_xy[src][1];
    action_prompt_float_set(pane[0] + 0x2c, active_xy[dst][0]);
    action_prompt_float_set(pane[0] + 0x30, active_xy[dst][1]);
    bool left = dst == 2;
    action_prompt_float_set(pane[1] + 0x2c, active_xy[dst][0] + (left ? -60 : 60));
    action_prompt_float_set(pane[1] + 0x30, active_xy[dst][1] + capture_y);
    for (int i = 0; i < 4; i++) {
        if (i == active) continue;
        action_prompt_float_set(pane[3 + i] + 0x2c, icon_xy[positions[i]][0]);
        action_prompt_float_set(pane[3 + i] + 0x30, icon_xy[positions[i]][1]);
    }
    memcpy(base, dst == 0 || dst == 3 ? base_b : base_a, sizeof base_a);
    action_prompt_float_set(pane[2] + 0x40, dst == 2 || dst == 3 ? 180 : 0);
    action_prompt_float_set(pane[2] + 0x44, dst == 3 ? -1 : 1);
    // The game's left-facing bar is mirrored, but the text remains readable and right-aligned.
    action_prompt_float_set(pane[7] + 0x2c, left ? 37 : -39);
    action_prompt_float_set(pane[8] + 0x2c, left ? -23 : 21);
    pane[7][9] = left ? 2 : 1;
    pane[8][9] = left ? 2 : 1;
    pane[7][0x5c] = left ? 2 : 1;
    pane[8][0x5c] = left ? 2 : 1;
    action_prompt_float_set(pane[11] + 0x2c, left ? -60 : 60);
    action_prompt_float_set(pane[9] + 0x2c, left ? 60 : -60);
    action_prompt_float_set(pane[10] + 0x2c, left ? 33 : -33);
    action_prompt_float_set(pane[10] + 0x44, left ? -1 : 1);
    if (active == 3)
        for (int i = 12; i < 15; i++) action_prompt_float_set(pane[i] + 0x2c, left ? -86 : 34);
    return true;
}
