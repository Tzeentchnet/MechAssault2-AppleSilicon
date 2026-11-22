/*
 * POSIX SDL framebuffer host primitives, adapted from xboxrecomp v0.18.0
 * src/video/fb_present_sdl.c (commit 37b61d589168adee3680fabd51805ee81427bf07).
 * This is a main-thread window and frame handoff, not an NV2A renderer.
 */
#if !defined(_WIN32)
#include "framebuffer_host.h"
#include "../input/xinput_xbox.h"
#include "../kernel/xbox_memory_layout.h"
#include "../platform/pc_frontend.h"
#include "../platform/pc_switch.h"
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mcpx_apu_set_background(int background);

typedef struct FrameSlot {
    uint8_t *pixels;
    size_t capacity, pitch;
    unsigned width, height;
    XboxFramebufferFormat format;
} FrameSlot;

static FrameSlot s_slots[3];
static int s_back, s_ready = 1, s_front = 2;
static uint64_t s_published, s_consumed;
static pthread_mutex_t s_writer = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_title_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_running, s_close_requested, s_mouse_captured;
static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static unsigned s_texture_w, s_texture_h;
static XboxFramebufferFormat s_texture_format;
static Uint32 s_owned_sdl, s_frame_event;
static char s_title[128] = "Xbox Recomp";
static int s_title_dirty;
static uint32_t s_fb_va, s_fb_pitch;
static volatile LONG s_flips;
static unsigned char s_key_down[256];
static int s_key_pending[256], s_mouse_dx, s_mouse_dy;
static unsigned s_modifiers;
static const char *s_ui_script;
static Uint32 s_ui_started;
static const char *s_ui_live;
static long s_ui_live_offset;
static Uint32 s_ui_live_poll;

int xbox_FramebufferSdlKeyToVk(int key)
{
    if (key >= SDLK_a && key <= SDLK_z) return key - SDLK_a + 'A';
    if (key >= SDLK_0 && key <= SDLK_9) return key;
    if (key >= SDLK_F1 && key <= SDLK_F12) return VK_F1 + key - SDLK_F1;
    switch (key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return VK_RETURN;
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB: return VK_TAB;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return VK_CONTROL;
    case SDLK_LALT: case SDLK_RALT: return VK_MENU;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_KP_0: return VK_NUMPAD0;
    case SDLK_KP_1: return VK_NUMPAD1;
    case SDLK_KP_2: return VK_NUMPAD2;
    case SDLK_KP_3: return VK_NUMPAD3;
    case SDLK_KP_4: return VK_NUMPAD4;
    case SDLK_KP_5: return VK_NUMPAD5;
    case SDLK_KP_6: return VK_NUMPAD6;
    case SDLK_KP_7: return VK_NUMPAD7;
    case SDLK_KP_8: return VK_NUMPAD8;
    case SDLK_KP_9: return VK_NUMPAD9;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
    case SDLK_KP_PLUS: return VK_ADD;
    case SDLK_KP_MINUS: return VK_SUBTRACT;
    case SDLK_KP_PERIOD: return VK_DECIMAL;
    case SDLK_KP_DIVIDE: return VK_DIVIDE;
    case SDLK_HOME: return VK_HOME;
    case SDLK_END: return VK_END;
    case SDLK_PAGEUP: return VK_PRIOR;
    case SDLK_PAGEDOWN: return VK_NEXT;
    case SDLK_INSERT: return VK_INSERT;
    case SDLK_DELETE: return VK_DELETE;
    case SDLK_MINUS: return VK_OEM_MINUS;
    case SDLK_EQUALS: return VK_OEM_PLUS;
    case SDLK_COMMA: return VK_OEM_COMMA;
    case SDLK_PERIOD: return VK_OEM_PERIOD;
    case SDLK_SEMICOLON: return VK_OEM_1;
    case SDLK_SLASH: return VK_OEM_2;
    case SDLK_BACKQUOTE: return VK_OEM_3;
    case SDLK_LEFTBRACKET: return VK_OEM_4;
    case SDLK_BACKSLASH: return VK_OEM_5;
    case SDLK_RIGHTBRACKET: return VK_OEM_6;
    case SDLK_QUOTE: return VK_OEM_7;
    case SDLK_CAPSLOCK: return VK_CAPITAL;
    case SDLK_PAUSE: return VK_PAUSE;
    default: return 0;
    }
}

static void key_set(int vk, int down)
{
    if (vk <= 0 || vk > 255) return;
    __atomic_store_n(&s_key_down[vk], (unsigned char)(down != 0), __ATOMIC_RELEASE);
    if (down) __atomic_store_n(&s_key_pending[vk], 1, __ATOMIC_RELEASE);
}

static void keys_clear(void)
{
    s_modifiers = 0;
    for (int i = 0; i < 256; ++i) {
        __atomic_store_n(&s_key_down[i], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_key_pending[i], 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s_mouse_dx, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mouse_dy, 0, __ATOMIC_RELEASE);
}

static void diagnostic_action(const char *action)
{
    if (strcmp(action, "close") == 0) {
        s_close_requested = 1;
        return;
    }
    if (strncmp(action, "key=", 4) == 0) {
        char *ending;
        unsigned key = (unsigned)strtoul(action + 4, &ending, 0);
        if (key > 0 && key < 256) {
            if (strcmp(ending, "down") == 0) key_set((int)key, 1);
            else if (strcmp(ending, "up") == 0) key_set((int)key, 0);
            else if (!*ending) {
                key_set((int)key, 1);
                key_set((int)key, 0);
            } else key = 0;
            if (key) {
                pc_input_note_keyboard();
                return;
            }
        }
    }
    fprintf(stderr, "[FB] invalid SDL diagnostic action\n");
    s_ui_script = NULL;
}

static void pump_diagnostic_script(void)
{
    while (s_ui_script && *s_ui_script) {
        unsigned at = 0;
        int consumed = 0;
        char action[64] = {0};
        if (sscanf(s_ui_script, "%u:%63[^,]%n", &at, action, &consumed) < 2 ||
            consumed <= 0) {
            fprintf(stderr, "[FB] invalid SDL diagnostic script\n");
            s_ui_script = NULL;
            return;
        }
        if ((Uint32)(SDL_GetTicks() - s_ui_started) < at) return;
        diagnostic_action(action);
        if (!s_ui_script) return;
        s_ui_script += consumed;
        if (*s_ui_script == ',') ++s_ui_script;
    }
}

static void pump_diagnostic_live(void)
{
    Uint32 now=SDL_GetTicks();
    if(!s_ui_live || !*s_ui_live || (Uint32)(now-s_ui_live_poll)<50)return;
    s_ui_live_poll=now;
    FILE *commands=fopen(s_ui_live,"rb");
    if(!commands)return;
    if(fseek(commands,s_ui_live_offset,SEEK_SET)){
        fclose(commands);
        return;
    }
    char action[128];
    while(fgets(action,sizeof(action),commands)){
        char *newline=strchr(action,'\n');
        if(!newline)break;
        long next=ftell(commands);
        if(next<0)break;
        s_ui_live_offset=next;
        *newline=0;
        size_t length=strlen(action);
        if(length && action[length-1]=='\r')action[length-1]=0;
        diagnostic_action(action);
    }
    fclose(commands);
}

static int modifier_set(int key, int down)
{
    unsigned bit, group;
    int generic, side;
    switch (key) {
    case SDLK_LSHIFT: bit = 1u; group = 3u; generic = VK_SHIFT; side = VK_LSHIFT; break;
    case SDLK_RSHIFT: bit = 2u; group = 3u; generic = VK_SHIFT; side = VK_RSHIFT; break;
    case SDLK_LCTRL: bit = 4u; group = 12u; generic = VK_CONTROL; side = VK_LCONTROL; break;
    case SDLK_RCTRL: bit = 8u; group = 12u; generic = VK_CONTROL; side = VK_RCONTROL; break;
    case SDLK_LALT: bit = 16u; group = 48u; generic = VK_MENU; side = VK_LMENU; break;
    case SDLK_RALT: bit = 32u; group = 48u; generic = VK_MENU; side = VK_RMENU; break;
    default: return 0;
    }
    unsigned old = s_modifiers;
    if (down) s_modifiers |= bit;
    else s_modifiers &= ~bit;
    key_set(side, down);
    if ((old & group) != 0 && (s_modifiers & group) == 0) key_set(generic, 0);
    if ((old & group) == 0 && (s_modifiers & group) != 0) key_set(generic, 1);
    return 1;
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk >= 256) return 0;
    const char *latch = getenv("RECOMP_INPUT_LATCH");
    return __atomic_load_n(&s_key_down[vk], __ATOMIC_ACQUIRE) ||
        (latch && atoi(latch) != 0 &&
         __atomic_load_n(&s_key_pending[vk], __ATOMIC_ACQUIRE));
}

void xbox_FramebufferAcknowledgeKey(int vk)
{
    if ((unsigned)vk < 256) __atomic_store_n(&s_key_pending[vk], 0, __ATOMIC_RELEASE);
}

void xbox_FramebufferMouseDelta(int *dx, int *dy)
{
    if (dx) *dx = __atomic_exchange_n(&s_mouse_dx, 0, __ATOMIC_ACQ_REL);
    if (dy) *dy = __atomic_exchange_n(&s_mouse_dy, 0, __ATOMIC_ACQ_REL);
}

int xbox_FramebufferHostMouseCapture(int capture)
{
    if (!s_window) return 0;
    if (capture && pc_frontend_captured()) return 0;
    if (SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE) != 0) {
        if (!capture) {
            s_mouse_captured = 0;
            __atomic_store_n(&s_mouse_dx, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&s_mouse_dy, 0, __ATOMIC_RELEASE);
            pc_input_camera_capture(0);
        }
        fprintf(stderr, "[FB] SDL mouse capture failed: %s\n", SDL_GetError());
        return 0;
    }
    s_mouse_captured = capture != 0;
    __atomic_store_n(&s_mouse_dx, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mouse_dy, 0, __ATOMIC_RELEASE);
    pc_input_camera_capture(s_mouse_captured && pc_input_context() == PC_INPUT_MENU);
    return 1;
}

static void handle_event(const SDL_Event *event)
{
    if (event->type == SDL_QUIT) {
        s_close_requested = 1;
    } else if (event->type == SDL_WINDOWEVENT) {
        if (event->window.windowID != SDL_GetWindowID(s_window)) return;
        if (event->window.event == SDL_WINDOWEVENT_CLOSE) s_close_requested = 1;
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
            event->window.event == SDL_WINDOWEVENT_HIDDEN ||
            event->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            keys_clear();
            xbox_FramebufferHostMouseCapture(0);
            xbox_InputFeedbackFocus(FALSE);
            if (pc_switch("RECOMP_MUTE_IN_BACKGROUND", "Audio", "MuteInBackground", 1))
                mcpx_apu_set_background(1);
            if (pc_switch("RECOMP_PAUSE_ON_FOCUS_LOSS", "Input", "PauseOnFocusLoss", 1))
                xbox_InputPauseTap("window lost focus");
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            xbox_InputFeedbackFocus(TRUE);
            mcpx_apu_set_background(0);
        } else if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            pc_display_size((unsigned)event->window.data1, (unsigned)event->window.data2);
        }
    } else if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        if (event->key.windowID != SDL_GetWindowID(s_window)) return;
        int down = event->type == SDL_KEYDOWN;
        if (down && event->key.repeat) return;
        if (down && (event->key.keysym.sym == SDLK_LGUI ||
                     event->key.keysym.sym == SDLK_RGUI)) {
            keys_clear();
            return;
        }
        if (down && (event->key.keysym.mod & KMOD_GUI)) return;
        if (!modifier_set(event->key.keysym.sym, down))
            key_set(xbox_FramebufferSdlKeyToVk(event->key.keysym.sym), down);
        if (down) pc_input_note_keyboard();
    } else if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
        if (event->button.windowID != SDL_GetWindowID(s_window)) return;
        int vk = event->button.button == SDL_BUTTON_LEFT ? VK_LBUTTON :
                 event->button.button == SDL_BUTTON_RIGHT ? VK_RBUTTON :
                 event->button.button == SDL_BUTTON_MIDDLE ? VK_MBUTTON :
                 event->button.button == SDL_BUTTON_X1 ? VK_XBUTTON1 :
                 event->button.button == SDL_BUTTON_X2 ? VK_XBUTTON2 : 0;
        int down = event->type == SDL_MOUSEBUTTONDOWN;
        key_set(vk, down);
        if (down) pc_input_note_keyboard();
    } else if (event->type == SDL_MOUSEMOTION && s_mouse_captured &&
               event->motion.windowID == SDL_GetWindowID(s_window) &&
               !pc_frontend_captured() && pc_setting_get(PC_MOUSE_AIM, 1)) {
        __atomic_add_fetch(&s_mouse_dx, event->motion.xrel, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_mouse_dy, event->motion.yrel, __ATOMIC_RELAXED);
        if (event->motion.xrel || event->motion.yrel) pc_input_note_keyboard();
    }
}

int xbox_FramebufferSubmit(const void *pixels, unsigned width, unsigned height,
                           size_t pitch, XboxFramebufferFormat format)
{
    size_t bpp = format == XBOX_FRAMEBUFFER_X8R8G8B8 ? 4 :
                 format == XBOX_FRAMEBUFFER_RGB565 ? 2 : 0;
    if (!pixels || !width || !height || width > 8192 || height > 8192 ||
        !bpp || pitch < (size_t)width * bpp ||
        pitch > SIZE_MAX / height || !__atomic_load_n(&s_running, __ATOMIC_ACQUIRE))
        return 0;
    size_t row = (size_t)width * bpp, bytes = row * height;
    pthread_mutex_lock(&s_writer);
    if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&s_writer);
        return 0;
    }
    FrameSlot *slot = &s_slots[s_back];
    if (slot->capacity < bytes) {
        void *memory = realloc(slot->pixels, bytes);
        if (!memory) {
            fprintf(stderr, "[FB] cannot allocate %zu-byte frame\n", bytes);
            pthread_mutex_unlock(&s_writer);
            return 0;
        }
        slot->pixels = memory;
        slot->capacity = bytes;
    }
    for (unsigned y = 0; y < height; ++y)
        memcpy(slot->pixels + y * row, (const uint8_t *)pixels + y * pitch, row);
    slot->width = width; slot->height = height; slot->pitch = row; slot->format = format;
    pthread_mutex_lock(&s_frame_lock);
    int old = s_ready; s_ready = s_back; s_back = old;
    ++s_published;
    pthread_mutex_unlock(&s_frame_lock);

    SDL_Event event;
    SDL_zero(event);
    event.type = s_frame_event;
    if (SDL_PushEvent(&event) < 0)
        fprintf(stderr, "[FB] could not wake event pump: %s\n", SDL_GetError());
    pthread_mutex_unlock(&s_writer);
    return 1;
}

int xbox_FramebufferHostOpen(const char *title, unsigned width, unsigned height)
{
#if defined(__APPLE__)
    if (!pthread_main_np()) {
        fprintf(stderr, "[FB] SDL video must be opened on the process main thread\n");
        return 0;
    }
#endif
    if (s_window || !width || !height || width > 8192 || height > 8192) return 0;
    Uint32 wanted = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    s_owned_sdl = wanted & ~SDL_WasInit(wanted);
    if (s_owned_sdl && SDL_InitSubSystem(s_owned_sdl) != 0) {
        fprintf(stderr, "[FB] SDL video initialization failed: %s\n", SDL_GetError());
        s_owned_sdl = 0;
        return 0;
    }
    s_frame_event = SDL_RegisterEvents(1);
    if (s_frame_event == (Uint32)-1) {
        fprintf(stderr, "[FB] could not register frame event: %s\n", SDL_GetError());
        goto failed;
    }
    s_window = SDL_CreateWindow(title ? title : s_title, SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, (int)width, (int)height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_window) {
        fprintf(stderr, "[FB] SDL window creation failed: %s\n", SDL_GetError());
        goto failed;
    }
    s_renderer = SDL_CreateRenderer(s_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        fprintf(stderr, "[FB] accelerated SDL presentation unavailable; trying software: %s\n",
                SDL_GetError());
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_renderer) {
        fprintf(stderr, "[FB] SDL renderer creation failed: %s\n", SDL_GetError());
        goto failed;
    }
    s_close_requested = 0;
    s_ui_script = getenv("RECOMP_UI_SCRIPT");
    s_ui_started = SDL_GetTicks();
    s_ui_live = getenv("RECOMP_UI_LIVE");
    s_ui_live_offset = 0;
    s_ui_live_poll = s_ui_started;
    keys_clear();
    SDL_StopTextInput();
    xbox_InputSetMainPump(1);
    pc_display_size(width, height);
    __atomic_store_n(&s_running, 1, __ATOMIC_RELEASE);
    return 1;
failed:
    xbox_FramebufferHostClose();
    return 0;
}

int xbox_FramebufferHostPump(unsigned timeout_ms)
{
#if defined(__APPLE__)
    if (!pthread_main_np()) return -1;
#endif
    if (!s_window || !s_renderer) return -1;
    pump_diagnostic_script();
    pump_diagnostic_live();
    if (s_mouse_captured && pc_frontend_captured()) {
        xbox_FramebufferHostMouseCapture(0);
        keys_clear();
    }
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, (int)(timeout_ms > 1000 ? 1000 : timeout_ms))) {
        handle_event(&event);
        while (SDL_PollEvent(&event)) handle_event(&event);
    }
    if (s_close_requested) return 0;

    pthread_mutex_lock(&s_title_lock);
    if (s_title_dirty) { SDL_SetWindowTitle(s_window, s_title); s_title_dirty = 0; }
    pthread_mutex_unlock(&s_title_lock);
    pthread_mutex_lock(&s_frame_lock);
    int available = s_consumed != s_published;
    if (available) {
        int old = s_front; s_front = s_ready; s_ready = old;
        s_consumed = s_published;
    }
    FrameSlot frame = s_slots[s_front];
    pthread_mutex_unlock(&s_frame_lock);
    if (!available) return 1;

    if (!s_texture || frame.width != s_texture_w || frame.height != s_texture_h ||
        frame.format != s_texture_format) {
        if (s_texture) SDL_DestroyTexture(s_texture);
        Uint32 format = frame.format == XBOX_FRAMEBUFFER_RGB565 ?
            SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB8888;
        s_texture = SDL_CreateTexture(s_renderer, format, SDL_TEXTUREACCESS_STREAMING,
                                      (int)frame.width, (int)frame.height);
        if (!s_texture) {
            fprintf(stderr, "[FB] SDL texture creation failed: %s\n", SDL_GetError());
            return -1;
        }
        if (SDL_SetTextureBlendMode(s_texture, SDL_BLENDMODE_NONE) != 0 ||
            SDL_RenderSetLogicalSize(s_renderer, (int)frame.width, (int)frame.height) != 0) {
            fprintf(stderr, "[FB] SDL texture setup failed: %s\n", SDL_GetError());
            SDL_DestroyTexture(s_texture);
            s_texture = NULL;
            return -1;
        }
        s_texture_w = frame.width; s_texture_h = frame.height; s_texture_format = frame.format;
    }
    if (SDL_UpdateTexture(s_texture, NULL, frame.pixels, (int)frame.pitch) != 0 ||
        SDL_RenderClear(s_renderer) != 0 ||
        SDL_RenderCopy(s_renderer, s_texture, NULL, NULL) != 0) {
        fprintf(stderr, "[FB] SDL presentation failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_RenderPresent(s_renderer);
    return 1;
}

void xbox_FramebufferHostClose(void)
{
#if defined(__APPLE__)
    if (!pthread_main_np()) {
        fprintf(stderr, "[FB] SDL video must be closed on the process main thread\n");
        return;
    }
#endif
    __atomic_store_n(&s_running, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s_writer);
    pthread_mutex_lock(&s_frame_lock);
    for (int i = 0; i < 3; ++i) {
        free(s_slots[i].pixels);
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
    }
    s_back = 0; s_ready = 1; s_front = 2; s_published = s_consumed = 0;
    pthread_mutex_unlock(&s_frame_lock);
    pthread_mutex_unlock(&s_writer);
    if (s_window && s_mouse_captured) xbox_FramebufferHostMouseCapture(0);
    if (s_texture) SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window) SDL_DestroyWindow(s_window);
    s_texture = NULL; s_renderer = NULL; s_window = NULL;
    s_texture_w = s_texture_h = 0;
    s_ui_script = NULL;
    s_ui_started = 0;
    s_ui_live = NULL;
    s_ui_live_offset = 0;
    s_ui_live_poll = 0;
    xbox_InputSetMainPump(0);
    keys_clear();
    if (s_owned_sdl) SDL_QuitSubSystem(s_owned_sdl);
    s_owned_sdl = 0;
}

void *xbox_FramebufferWindowHandle(void) { return s_window; }
void xbox_FramebufferWindowHardware(int enabled) { (void)enabled; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch)
{
    __atomic_store_n(&s_fb_va, va, __ATOMIC_RELEASE);
    if (pitch) __atomic_store_n(&s_fb_pitch, pitch, __ATOMIC_RELEASE);
}
void xbox_FramebufferWindowStart(void)
{
    if (getenv("RECOMP_FB_WINDOW") && !__atomic_load_n(&s_running, __ATOMIC_ACQUIRE))
        fprintf(stderr, "[FB] open the SDL host window on the main thread before starting the guest\n");
}
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE) || !va || !pitch) return;
    const char *pin = getenv("RECOMP_FB_VA");
    if (pin) va = (uint32_t)strtoul(pin, NULL, 0);
    unsigned bpp = pitch / 640u;
    if (bpp != 2 && bpp != 4) {
        fprintf(stderr, "[FB] unsupported guest framebuffer pitch %u\n", pitch);
        return;
    }
    size_t bytes = (size_t)479 * pitch + (size_t)640 * bpp;
    const void *pixels = xbox_TranslateMappedRam(va, bytes);
    if (!pixels || !xbox_FramebufferSubmit(pixels, 640, 480, pitch,
                 bpp == 2 ? XBOX_FRAMEBUFFER_RGB565 : XBOX_FRAMEBUFFER_X8R8G8B8))
        fprintf(stderr, "[FB] guest frame 0x%08X could not be submitted\n", va);
}
void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    (void)draws;
    InterlockedIncrement(&s_flips);
}
LONG xbox_FramebufferFlipCount(void) { return InterlockedCompareExchange(&s_flips, 0, 0); }

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    if (!name || max_chars <= 0) return;
    char text[sizeof(s_title)];
    size_t out = 0;
    for (int i = 0; i < max_chars && name[i]; ++i) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < max_chars &&
            name[i + 1] >= 0xDC00 && name[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (name[++i] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (out + n >= sizeof(text)) break;
        if (n == 1) text[out++] = (char)c;
        else {
            if (n == 4) text[out++] = (char)(0xF0 | (c >> 18));
            if (n >= 3) text[out++] = (char)((n == 3 ? 0xE0 : 0x80) | ((c >> 12) & (n == 3 ? 0x0F : 0x3F)));
            text[out++] = (char)((n == 2 ? 0xC0 : 0x80) | ((c >> 6) & (n == 2 ? 0x1F : 0x3F)));
            text[out++] = (char)(0x80 | (c & 0x3F));
        }
    }
    text[out] = 0;
    pthread_mutex_lock(&s_title_lock);
    memcpy(s_title, text, out + 1);
    s_title_dirty = 1;
    pthread_mutex_unlock(&s_title_lock);
}
#endif
