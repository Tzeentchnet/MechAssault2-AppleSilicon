#include "host_shell.h"

#include "video/framebuffer_host.h"
#include <SDL.h>

/* The runtime exposes this handle for its existing host integration. */
void *xbox_FramebufferWindowHandle(void);

Ma2HostWindowResult ma2_host_window_open(Ma2HostWindow *window,
                                         const Ma2HostSettings *settings,
                                         Ma2HostPresentation *presentation)
{
    if (!pthread_main_np()) return MA2_WINDOW_WRONG_THREAD;
    if (!window || !settings || !presentation || window->open) return MA2_WINDOW_ERROR;
    window->sdl_before = SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    if (SDL_SetHintWithPriority(SDL_HINT_NO_SIGNAL_HANDLERS, "1", SDL_HINT_OVERRIDE) != SDL_TRUE)
        return MA2_WINDOW_ERROR;
    if (!xbox_FramebufferHostOpen(
            "MechAssault 2 - host shell (guest and rendering not implemented)",
            settings->width, settings->height))
        return MA2_WINDOW_ERROR;

    SDL_Window *sdl_window = xbox_FramebufferWindowHandle();
    SDL_Renderer *renderer = sdl_window ? SDL_GetRenderer(sdl_window) : NULL;
    SDL_RendererInfo info;
    if (!renderer || SDL_GetRendererInfo(renderer, &info) != 0 ||
        !(info.flags & (SDL_RENDERER_SOFTWARE | SDL_RENDERER_ACCELERATED))) {
        xbox_FramebufferHostClose();
        return MA2_WINDOW_ERROR;
    }
    *presentation = (info.flags & SDL_RENDERER_ACCELERATED) ?
        MA2_PRESENT_ACCELERATED : MA2_PRESENT_SOFTWARE;
    window->open = 1;
    return MA2_WINDOW_RUNNING;
}

Ma2HostWindowResult ma2_host_window_pump(Ma2HostWindow *window, unsigned timeout_ms)
{
    if (!pthread_main_np()) return MA2_WINDOW_WRONG_THREAD;
    if (!window || !window->open) return MA2_WINDOW_ERROR;
    int result = xbox_FramebufferHostPump(timeout_ms);
    return result > 0 ? MA2_WINDOW_RUNNING :
        result == 0 ? MA2_WINDOW_CLOSED : MA2_WINDOW_ERROR;
}

Ma2HostWindowResult ma2_host_window_close(Ma2HostWindow *window)
{
    if (!pthread_main_np()) return MA2_WINDOW_WRONG_THREAD;
    if (!window || !window->open) return MA2_WINDOW_ERROR;
    xbox_FramebufferHostClose();
    window->open = 0;
    if (xbox_FramebufferWindowHandle() ||
        SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != window->sdl_before)
        return MA2_WINDOW_ERROR;
    return MA2_WINDOW_CLOSED;
}
