#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/services/action_prompt_layout.h"
#include "../runtime/services/action_prompt_restore.h"

static const char *const group[] = {"N_BtnA_00", "N_BtnB_00", "N_BtnX_00", "N_BtnX_00"};
static const char *const capture[] = {"N_CaptureA_00", "N_CaptureB_00", "N_CaptureX_00", "N_CaptureY_00"};
static const char *const plate[] = {"P_Base_00", "P_BaseB_00", "P_BaseX_00", "P_BaseY_00"};
static const char *const letters[4][4] = {
    {NULL, "T_B_00", "T_X_00", "T_Y_00"}, {"T_A_01", NULL, "T_X_00", "T_Y_00"},
    {"T_A_00", "T_B_00", NULL, "T_Y_00"}, {"T_A_00", "T_B_00", "T_X_00", NULL},
};
static const char *const guide0[] = {"T_GuideA_00", "T_GuideB_00", "T_GuideX_00", "T_GuideY_00"};
static const char *const guide1[] = {"T_GuideA_01", "T_GuideB_01", "T_GuideX_01", "T_GuideY_01"};
static const char *const icon1[] = {"Nt_KeyTexA_01", "Nt_KeyTexB_01", "Nt_KeyTexX_01", "Nt_KeyTexY_01"};
static const char *const window[] = {"W_BaseA_00", "W_BaseB_00", "W_BaseX_00", "W_BaseY_00"};
static const int original[] = {1, 0, 3, 2};
static const float textxy[4][2] = {{1, -26}, {26.25f, 0.75f}, {-25.5f, -0.25f}, {0.75f, 26}};
static const float btnxy[4][2] = {{0, -26}, {26, 0}, {-26, 0}, {0, 26}};

static uint8_t *add(uint8_t *p, const char *name, float x, float y) {
    uint32_t size = 0x88;
    memcpy(p, "pic1", 4);
    memcpy(p + 4, &size, 4);
    memcpy(p + 12, name, strlen(name) + 1);
    memcpy(p + 0x2c, &x, 4);
    memcpy(p + 0x30, &y, 4);
    return p + size;
}
static void test_one(int index, const int mapping[4]) {
    uint8_t buf[0x1000] = {0};
    memcpy(buf, "FLYT\xff\xfe\x14", 7);
    uint8_t *p = buf + 0x14;
    uint8_t *g = p;
    p = add(p, group[index], btnxy[original[index]][0], btnxy[original[index]][1]);
    uint8_t *cap = p;
    p = add(p, capture[index], 0, btnxy[original[index]][1] + (index == 0 ? 4 : 0));
    uint8_t *bg = p;
    p = add(p, plate[index], 0, 0);
    uint8_t *texts[4] = {0};
    for (int j = 0; j < 4; j++) {
        if (j == index) continue;
        texts[j] = p;
        p = add(p, letters[index][j], textxy[original[j]][0], textxy[original[j]][1]);
    }
    uint8_t *label0 = p;
    p = add(p, guide0[index], 0, 0);
    uint8_t *label1 = p;
    p = add(p, guide1[index], 0, 0);
    p = add(p, icon1[index], 0, 0);
    uint8_t *bar = p;
    p = add(p, window[index], 0, 0);
    p = add(p, index == 0 ? "P_CaptureUse00_00" : "P_CaptureUse_02", 0, 0);
    if (index == 3) {
        p = add(p, "P_CaptureUse00_03", -86, 0);
        p = add(p, "P_CaptureUse00_04", -86, 0);
        p = add(p, "P_CaptureUse00_05", -86, 0);
    }
    uint32_t size = 0x20;
    memcpy(p, "txl1", 4);
    memcpy(p + 4, &size, 4);
    memcpy(p + 8, index == 0 || index == 3 ? "Nt_KeyBtnSubBaseAL_00^s" : "Nt_KeyBtnSubBaseBL_00^s", 24);
    uint8_t *texture = p + 8;
    p += size;
    assert(action_prompt_layout(buf, p - buf, index, mapping));
    int dst = mapping[index];
    assert(action_prompt_float(g + 0x2c) == btnxy[dst][0]);
    assert(action_prompt_float(g + 0x30) == btnxy[dst][1]);
    assert(action_prompt_float(cap + 0x2c) == btnxy[dst][0] + (dst == 2 ? -60 : 60));
    assert(action_prompt_float(cap + 0x30) == btnxy[dst][1] + (index == 0 ? 4 : 0));
    assert(action_prompt_float(label0 + 0x2c) == (dst == 2 ? 37 : -39));
    assert(action_prompt_float(label1 + 0x2c) == (dst == 2 ? -23 : 21));
    assert(action_prompt_float(bar + 0x2c) == (dst == 2 ? 33 : -33));
    assert(action_prompt_float(bar + 0x44) == (dst == 2 ? -1 : 1));
    assert(action_prompt_float(bg + 0x40) == (dst == 2 || dst == 3 ? 180 : 0));
    assert(!memcmp(texture, dst == 0 || dst == 3 ? "Nt_KeyBtnSubBaseBL_00^s" : "Nt_KeyBtnSubBaseAL_00^s", 24));
    for (int j = 0; j < 4; j++) if (texts[j]) {
        assert(action_prompt_float(texts[j] + 0x2c) == textxy[mapping[j]][0]);
        assert(action_prompt_float(texts[j] + 0x30) == textxy[mapping[j]][1]);
    }
}
int main(int argc, char **argv) {
    const int layouts[][4] = {{1, 0, 3, 2}, {0, 1, 2, 3}, {0, 1, 3, 2}, {1, 0, 2, 3}};
    if (argc == 5) {
        for (int i = 0; i < 4; i++) {
            FILE *file = fopen(argv[i + 1], "rb");
            assert(file);
            uint8_t data[16384];
            size_t size = fread(data, 1, sizeof data, file);
            fclose(file);
            assert(action_prompt_layout(data, size, i, layouts[1]));
        }
    }
    for (unsigned k = 0; k < sizeof layouts / sizeof *layouts; k++)
        for (int i = 0; i < 4; i++) test_one(i, layouts[k]);
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++)
        for (int c = 0; c < 4; c++) for (int d = 0; d < 4; d++) {
            if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
            int mapping[] = {a, b, c, d};
            for (int i = 0; i < 4; i++) test_one(i, mapping);
        }
    uint64_t values[] = {0x222, 0x111, 0x222, 0x111};
    uint64_t original[] = {0x111, 0x222, 0x222, 0x111};
    uint64_t expected[] = {0x222, 0x111, 0x222, 0x111};
    uint64_t *slots[] = {&values[0], &values[1], &values[2], &values[3]};
    assert(action_prompt_restore_slots(slots, original, expected, 4) == 4);
    assert(!memcmp(values, original, sizeof values));
    return 0;
}
