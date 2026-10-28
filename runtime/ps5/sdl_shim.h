// The SDL calls the kept runtime code still makes on the PS5 (D2: a symbol needs a caller). The window is the
// VideoOut display plane of runtime/ps5/window.c; the input devices (DualSense, USB keyboard and mouse) are
// runtime/ps5/input.c; there is no audio device (M5).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct SDL_Window SDL_Window;

#define SDLCALL
#define SDL_INIT_AUDIO 0x00000010u
typedef uint32_t SDL_AudioDeviceID;
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xffffffffu)
typedef enum { SDL_AUDIO_S16 = 0x8010u, SDL_AUDIO_F32 = 0x8120u } SDL_AudioFormat;
typedef struct { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void (*SDL_AudioStreamCallback)(void *, SDL_AudioStream *, int, int);
bool SDL_InitSubSystem(uint32_t flags);
const char *SDL_GetError(void);
void ps5_sdl_error(const char *text);  // (runtime/ps5: what SDL_GetError says next; a string that stays)
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec *, SDL_AudioStreamCallback, void *);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID, SDL_AudioSpec *, int *);
float SDL_GetAudioStreamGain(SDL_AudioStream *);
bool SDL_SetAudioStreamGain(SDL_AudioStream *, float);
int SDL_GetAudioStreamQueued(SDL_AudioStream *);
bool SDL_PutAudioStreamData(SDL_AudioStream *, const void *, int);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *);
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *, SDL_AudioStreamCallback, void *);
void SDL_DestroyAudioStream(SDL_AudioStream *);

uint64_t SDL_GetTicks(void);

// ---- input devices (runtime/ps5/input.c): the subset runtime/hle/input.c, bindings.c and vibration.c read
typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t SDL_JoystickID;
typedef uint32_t SDL_Keycode;
typedef uint16_t SDL_Keymod;
#define SDL_KMOD_NONE 0x0000u
#define SDL_KMOD_LSHIFT 0x0001u
#define SDL_KMOD_RSHIFT 0x0002u
#define SDL_KMOD_LCTRL 0x0040u
#define SDL_KMOD_RCTRL 0x0080u
#define SDL_KMOD_LALT 0x0100u
#define SDL_KMOD_RALT 0x0200u
#define SDL_KMOD_LGUI 0x0400u
#define SDL_KMOD_RGUI 0x0800u
// Scancodes are USB HID keyboard usages, as SDL's are (sceKeyboard reports the same usages)
typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_C = 6, SDL_SCANCODE_D = 7, SDL_SCANCODE_E = 8, SDL_SCANCODE_F = 9,
    SDL_SCANCODE_I = 12, SDL_SCANCODE_J = 13, SDL_SCANCODE_K = 14, SDL_SCANCODE_L = 15, SDL_SCANCODE_M = 16,
    SDL_SCANCODE_Q = 20, SDL_SCANCODE_R = 21, SDL_SCANCODE_S = 22, SDL_SCANCODE_T = 23, SDL_SCANCODE_W = 26,
    SDL_SCANCODE_ESCAPE = 41, SDL_SCANCODE_TAB = 43, SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F8 = 65, SDL_SCANCODE_F9 = 66, SDL_SCANCODE_F11 = 68, SDL_SCANCODE_F12 = 69,
    SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_COUNT = 512
} SDL_Scancode;
const char *SDL_GetScancodeName(SDL_Scancode scancode);
SDL_Scancode SDL_GetScancodeFromName(const char *name);
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modstate, bool key_event);
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *modstate);
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_X1 4
#define SDL_BUTTON_X2 5
bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled);

typedef struct SDL_Gamepad SDL_Gamepad;
typedef enum {
    SDL_GAMEPAD_TYPE_UNKNOWN = 0, SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_XBOXONE,
    SDL_GAMEPAD_TYPE_PS3, SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_TYPE_PS5
} SDL_GamepadType;
typedef enum {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_MISC2, SDL_GAMEPAD_BUTTON_MISC3, SDL_GAMEPAD_BUTTON_MISC4,
    SDL_GAMEPAD_BUTTON_MISC5, SDL_GAMEPAD_BUTTON_MISC6, SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;
typedef enum {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;
typedef enum { SDL_SENSOR_ACCEL = 1, SDL_SENSOR_GYRO = 2 } SDL_SensorType;
SDL_JoystickID *SDL_GetGamepads(int *count);  // (SDL_free the list)
void SDL_free(void *mem);
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id);
void SDL_CloseGamepad(SDL_Gamepad *gamepad);
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad);
const char *SDL_GetGamepadName(SDL_Gamepad *gamepad);
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad);
SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID instance_id);
bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button);
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis);
bool SDL_GamepadHasSensor(SDL_Gamepad *gamepad, SDL_SensorType type);
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type, bool enabled);
float SDL_GetGamepadSensorDataRate(SDL_Gamepad *gamepad, SDL_SensorType type);
bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, uint16_t low, uint16_t high, uint32_t milliseconds);

// ---- video (runtime/ps5/window.c)
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef uint32_t SDL_DisplayID;
typedef uint32_t SDL_PropertiesID;
typedef uint64_t SDL_WindowFlags;
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_GAMEPAD 0x00002000u
#define SDL_WINDOW_FULLSCREEN 0x0000000000000001ull
#define SDL_WINDOW_HIDDEN 0x0000000000000008ull
#define SDL_WINDOW_MINIMIZED 0x0000000000000040ull
#define SDL_WINDOW_RESIZABLE 0x0000000000000020ull
#define SDL_WINDOW_HIGH_PIXEL_DENSITY 0x0000000000002000ull
#define SDL_WINDOW_VULKAN 0x0000000010000000ull
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
#define SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN "SDL.window.HDR_enabled"
#define SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT "SDL.window.SDR_white_level"
typedef struct {
    SDL_DisplayID displayID;
    uint32_t format;
    int w, h;
    float pixel_density;
    float refresh_rate;
    int refresh_rate_numerator, refresh_rate_denominator;
    void *internal;
} SDL_DisplayMode;
bool SDL_Init(uint32_t flags);
bool SDL_SetHint(const char *name, const char *value);
SDL_DisplayID SDL_GetPrimaryDisplay(void);
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID display);
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags);
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window);
const SDL_DisplayMode *SDL_GetWindowFullscreenMode(SDL_Window *window);
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window);
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value);
float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value);

// ---- events (runtime/ps5/window.c's queue): a quit request, and the input devices' events (runtime/ps5/input.c).
// The display plane has no window events. Types and field names are SDL3's; each kind keeps only what is read.
enum {
    SDL_EVENT_QUIT = 0x100,
    SDL_EVENT_WINDOW_FOCUS_GAINED = 0x20e, SDL_EVENT_WINDOW_FOCUS_LOST,
    SDL_EVENT_KEY_DOWN = 0x300, SDL_EVENT_KEY_UP, SDL_EVENT_TEXT_EDITING, SDL_EVENT_TEXT_INPUT,
    SDL_EVENT_MOUSE_MOTION = 0x400, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_MOUSE_WHEEL,
    SDL_EVENT_JOYSTICK_AXIS_MOTION = 0x600, SDL_EVENT_JOYSTICK_BUTTON_DOWN = 0x603, SDL_EVENT_JOYSTICK_BUTTON_UP,
    SDL_EVENT_JOYSTICK_ADDED,
    SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650, SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP,
    SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED, SDL_EVENT_GAMEPAD_SENSOR_UPDATE = 0x659,
};
#define SDL_EVENT_HEAD uint32_t type, reserved; uint64_t timestamp
typedef struct { SDL_EVENT_HEAD; uint32_t windowID; uint32_t which; SDL_Scancode scancode; SDL_Keycode key;
                 SDL_Keymod mod; bool down, repeat; } SDL_KeyboardEvent;
typedef struct { SDL_EVENT_HEAD; uint32_t windowID; const char *text; } SDL_TextInputEvent;
typedef struct { SDL_EVENT_HEAD; float xrel, yrel; } SDL_MouseMotionEvent;
typedef struct { SDL_EVENT_HEAD; Uint8 button; bool down; } SDL_MouseButtonEvent;
typedef struct { SDL_EVENT_HEAD; SDL_JoystickID which; } SDL_GamepadDeviceEvent;
typedef struct { SDL_EVENT_HEAD; SDL_JoystickID which; Uint8 axis; Sint16 value; } SDL_GamepadAxisEvent;
typedef struct { SDL_EVENT_HEAD; SDL_JoystickID which; Uint8 button; bool down; } SDL_GamepadButtonEvent;
typedef struct { SDL_EVENT_HEAD; SDL_JoystickID which; int32_t sensor; float data[3]; uint64_t sensor_timestamp; }
    SDL_GamepadSensorEvent;
#undef SDL_EVENT_HEAD
typedef union SDL_Event {
    uint32_t type;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_GamepadDeviceEvent gdevice;
    SDL_GamepadAxisEvent gaxis;
    SDL_GamepadButtonEvent gbutton;
    SDL_GamepadSensorEvent gsensor;
    uint8_t padding[128];
} SDL_Event;
bool SDL_WaitEventTimeout(SDL_Event *event, int32_t timeout_ms);
bool SDL_PollEvent(SDL_Event *event);
bool SDL_PushEvent(SDL_Event *event);  // (any thread)
void ps5_input_poll(void);  // runtime/ps5/input.c: the devices' new records as events (SDL_PollEvent, main thread)

// ---- timer and CPU
uint64_t SDL_GetTicksNS(void);
void SDL_Delay(uint32_t ms);
void SDL_DelayPrecise(uint64_t ns);
int SDL_GetNumLogicalCPUCores(void);
int SDL_GetSystemRAM(void);
