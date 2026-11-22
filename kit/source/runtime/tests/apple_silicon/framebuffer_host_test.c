#include "video/framebuffer_host.h"
#include "platform/win32_compat.h"
#include <SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *xbox_FramebufferWindowHandle(void);
extern void xbox_FramebufferAcknowledgeKey(int vk);
extern int xbox_FramebufferKeyDown(int vk);
extern void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);

static void *wrong_thread(void *unused)
{
    (void)unused;
    if (xbox_FramebufferHostOpen("bad thread", 32, 32)) return (void *)1;
    if (xbox_FramebufferHostPump(0) != -1) return (void *)1;
    xbox_FramebufferHostClose();
    return NULL;
}
static void *submit_thread(void *unused)
{
    (void)unused;
    uint32_t pixels[8];
    for (unsigned i = 0; i < 100; ++i) {
        for (int j = 0; j < 8; ++j) pixels[j] = 0xff000000u | (i << 8);
        if (!xbox_FramebufferSubmit(pixels, 4, 2, 4 * sizeof(uint32_t),
                                    XBOX_FRAMEBUFFER_X8R8G8B8)) return (void *)1;
    }
    return NULL;
}
static int key_event(Uint32 window_id, int key, int down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = window_id;
    e.key.keysym.sym = key;
    return SDL_PushEvent(&e) == 1;
}
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #expr, SDL_GetError()); return 1; } } while (0)

int main(void)
{
    CHECK(!xbox_FramebufferSubmit(NULL, 4, 2, 16, XBOX_FRAMEBUFFER_X8R8G8B8));
    CHECK(xbox_FramebufferHostOpen("synthetic host", 64, 32));
    SDL_Window *window = (SDL_Window *)xbox_FramebufferWindowHandle();
    CHECK(window && SDL_GetRenderer(window));
    const uint16_t title[] = {'T', 0x00e9, 0xd83d, 0xde80, 0};
    xbox_FramebufferWindowSetTitle(title, 5);
    CHECK(xbox_FramebufferHostPump(0) == 1);
    CHECK(strcmp(SDL_GetWindowTitle(window), "T\xc3\xa9\xf0\x9f\x9a\x80") == 0);
    pthread_t thread;
    void *result;
    CHECK(pthread_create(&thread, NULL, wrong_thread, NULL) == 0);
    CHECK(pthread_join(thread, &result) == 0 && !result);
    CHECK(xbox_FramebufferWindowHandle() == window);

    uint32_t padded[10] = {0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000, 0,
                           0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000, 0};
    CHECK(!xbox_FramebufferSubmit(padded, 4, 2, 15, XBOX_FRAMEBUFFER_X8R8G8B8));
    CHECK(!xbox_FramebufferSubmit(padded, 4, 2, 20, (XboxFramebufferFormat)99));
    CHECK(xbox_FramebufferSubmit(padded, 4, 2, 20, XBOX_FRAMEBUFFER_X8R8G8B8));
    CHECK(pthread_create(&thread, NULL, submit_thread, NULL) == 0);
    CHECK(pthread_join(thread, &result) == 0 && !result);
    CHECK(xbox_FramebufferHostPump(10) == 1);
    uint32_t pixels[64 * 32];
    CHECK(SDL_RenderReadPixels(SDL_GetRenderer(window), NULL, SDL_PIXELFORMAT_ARGB8888,
                               pixels, 64 * (int)sizeof(uint32_t)) == 0);
    CHECK((pixels[0] & 0x00ffffff) == (99u << 8));

    uint16_t rgb565[8] = {0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800};
    CHECK(xbox_FramebufferSubmit(rgb565, 4, 2, 8, XBOX_FRAMEBUFFER_RGB565));
    CHECK(xbox_FramebufferHostPump(10) == 1);
    CHECK(SDL_RenderReadPixels(SDL_GetRenderer(window), NULL, SDL_PIXELFORMAT_ARGB8888,
                               pixels, 64 * (int)sizeof(uint32_t)) == 0);
    CHECK((pixels[0] & 0x00ff0000) == 0x00ff0000);

    CHECK(xbox_FramebufferSdlKeyToVk(SDLK_KP_7) == VK_NUMPAD7);
    CHECK(xbox_FramebufferSdlKeyToVk(SDLK_SEMICOLON) == VK_OEM_1);
    CHECK(xbox_FramebufferSdlKeyToVk(SDLK_LEFTBRACKET) == VK_OEM_4);
    Uint32 id = SDL_GetWindowID(window);
    CHECK(key_event(id, SDLK_LSHIFT, 1) && key_event(id, SDLK_RSHIFT, 1));
    CHECK(xbox_FramebufferHostPump(0) == 1);
    CHECK(xbox_FramebufferKeyDown(VK_SHIFT) && xbox_FramebufferKeyDown(VK_LSHIFT) &&
          xbox_FramebufferKeyDown(VK_RSHIFT));
    CHECK(key_event(id, SDLK_LSHIFT, 0) && xbox_FramebufferHostPump(0) == 1);
    CHECK(xbox_FramebufferKeyDown(VK_SHIFT) && !xbox_FramebufferKeyDown(VK_LSHIFT) &&
          xbox_FramebufferKeyDown(VK_RSHIFT));
    CHECK(key_event(id, SDLK_RSHIFT, 0) && xbox_FramebufferHostPump(0) == 1);
    CHECK(!xbox_FramebufferKeyDown(VK_SHIFT));

    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.windowID = id;
    event.button.button = SDL_BUTTON_X1;
    CHECK(SDL_PushEvent(&event) == 1 && xbox_FramebufferHostPump(0) == 1);
    CHECK(xbox_FramebufferKeyDown(VK_XBUTTON1));
    SDL_zero(event);
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = id;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    CHECK(SDL_PushEvent(&event) == 1 && xbox_FramebufferHostPump(0) == 1);
    CHECK(!xbox_FramebufferKeyDown(VK_XBUTTON1));
    xbox_FramebufferHostClose();
    CHECK(!xbox_FramebufferWindowHandle());
    CHECK(!xbox_FramebufferSubmit(padded, 4, 2, 20, XBOX_FRAMEBUFFER_X8R8G8B8));
    CHECK(setenv("RECOMP_INPUT_LATCH", "1", 1) == 0);
    CHECK(setenv("RECOMP_UI_SCRIPT", "50:key=13down,50:key=13up", 1) == 0);
    CHECK(xbox_FramebufferHostOpen("synthetic restart", 64, 32));
    CHECK(xbox_FramebufferHostPump(0) == 1);
    SDL_Delay(60);
    CHECK(xbox_FramebufferHostPump(0) == 1);
    CHECK(xbox_FramebufferKeyDown(VK_RETURN));
    xbox_FramebufferAcknowledgeKey(VK_RETURN);
    CHECK(!xbox_FramebufferKeyDown(VK_RETURN));
    xbox_FramebufferHostClose();
    CHECK(unsetenv("RECOMP_UI_SCRIPT") == 0);
    char live_path[] = "/tmp/ma2-ui-live-XXXXXX";
    int live_fd = mkstemp(live_path);
    CHECK(live_fd >= 0);
    CHECK(setenv("RECOMP_UI_LIVE", live_path, 1) == 0);
    CHECK(xbox_FramebufferHostOpen("synthetic live input", 64, 32));
    CHECK(xbox_FramebufferHostPump(0) == 1);
    CHECK(write(live_fd, "key=13\n", 7) == 7);
    SDL_Delay(60);
    CHECK(xbox_FramebufferHostPump(0) == 1);
    CHECK(xbox_FramebufferKeyDown(VK_RETURN));
    xbox_FramebufferAcknowledgeKey(VK_RETURN);
    CHECK(!xbox_FramebufferKeyDown(VK_RETURN));
    xbox_FramebufferHostClose();
    CHECK(close(live_fd) == 0);
    CHECK(unlink(live_path) == 0);
    CHECK(unsetenv("RECOMP_UI_LIVE") == 0);
    puts("SDL framebuffer: main thread, latest-frame copy, formats, and input passed");
    return 0;
}
