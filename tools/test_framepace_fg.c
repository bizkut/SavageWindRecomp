// runtime/hle/framepace.c on the host: the game frame's period with frame generation (docs/PS5-PORT-PLAN.md M7).
// Desktop's FSR 3 counts presented frames (the game draws at half the limit); the PS5's MihawkGen counts the game's
// frames, each presented twice: a 60 fps limit at 119.88 Hz keeps the game at 0.5 % under half the refresh rate
// (59.64 fps, presents 8.38 ms apart, so FIFO can drain), whatever the VSync setting's margin.
#include "../runtime/hle/framepace.c"

#include <math.h>

GameSites g_site;
Settings g_settings;
volatile int g_movie_playing, g_movie_fps;
uint64_t g_fs_vfr;
int16_t g_bind_pad[BIND_KEY_COUNT];
static float g_hz;
float winsize_refresh_rate(void) { return g_hz; }
bool movie_paced(void) { return true; }
bool nvn_timeline(void) { return false; }
void input_cam_frame(float ratio) { (void)ratio; }
uint32_t kernel_current_thread_handle(void) { return 0; }
uint64_t plat_now_ns(void) { return 0; }
uint64_t kernel_heap_size(void) { return 0; }
int input_pad_family(void) { return 0; }
int input_last_device(void) { return DEVICE_KEYBOARD; }
uint64_t renderer_present_count(void) { return 0; }

static uint32_t pause_state;  // (g_site.pause_state points here: playing)
static uint64_t event_mgr;  // no active event
static int failed;

static void expect_ms(const char *what, uint64_t ns, double ms) {
    bool ok = fabs(ns / 1e6 - ms) < 0.005;
    printf("%s %s: %.3f ms (want %.3f)\n", ok ? "ok  " : "FAIL", what, ns / 1e6, ms);
    failed += !ok;
}

static void set(int fps, int vsync, float hz, int active, int counts_game) {
    g_settings.fps_limit = fps, g_settings.vsync = vsync, g_hz = hz;
    g_fg_active = active, g_fg_counts_game = counts_game;
}

int main(void) {
    g_site.pause_state = (uint64_t)(uintptr_t)&pause_state;
    g_site.event_mgr = (uint64_t)(uintptr_t)&event_mgr;
    setenv("SAVAGEWIND_MOVIE_FPS", "0", 1);  // (no movie playing anyway)
    // frame generation off: one present per game frame, as in M6
    set(60, VSYNC_ON, 119.88f, 0, 0);
    expect_ms("off, 60 at 119.88 Hz", framepace_period_ns(), 1000 / 59.94);
    expect_ms("off: no generated presents to space", framepace_fg_period_ns(), 0);
    // MihawkGen: the limit counts the game's frames; ~120 presents at 119.88 Hz, half a period apart, 0.5 % under it
    const double most = 119.88 / 2 * 0.995;
    set(60, VSYNC_ON, 119.88f, 1, 1);
    expect_ms("MihawkGen, 60 at 119.88 Hz: game period", framepace_period_ns(), 1000 / most);
    expect_ms("MihawkGen: present spacing (half the period)", framepace_fg_period_ns() / 2, 500 / most);
    // no limit, or a limit over half the refresh rate: the game at that most (VSync's margin not applied)
    set(0, VSYNC_ON, 119.88f, 1, 1);
    expect_ms("MihawkGen, no limit at 119.88 Hz", framepace_period_ns(), 1000 / most);
    set(90, VSYNC_ON, 119.88f, 1, 1);
    expect_ms("MihawkGen, 90 at 119.88 Hz", framepace_period_ns(), 1000 / most);
    set(30, VSYNC_ON, 119.88f, 1, 1);
    expect_ms("MihawkGen, 30 at 119.88 Hz", framepace_period_ns(), 1000 / 29.97);
    // desktop's FSR 3 unchanged: the limit counts presented frames, the game at half of it
    set(120, VSYNC_OFF, 144.0f, 1, 0);
    expect_ms("FSR 3, 120 with VSync off", framepace_period_ns(), 1000 / 60.0);
    expect_ms("FSR 3: the present thread's own spacing", framepace_fg_period_ns(), 0);
    if (failed) printf("%d failed\n", failed);
    else printf("all passed\n");
    return failed != 0;
}
