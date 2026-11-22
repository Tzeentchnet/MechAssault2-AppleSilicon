/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/pc_frontend.h"
#include "../input/xinput_xbox.h"
#include "../platform/pc_switch.h"
void mcpx_apu_set_background(int background);   /* apu/apu_core.c (W-10) */

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static void *volatile s_fb_window;
static volatile LONG s_fb_hardware;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

void *xbox_FramebufferWindowHandle(void)
{
    return InterlockedCompareExchangePointer(&s_fb_window, NULL, NULL);
}

void xbox_FramebufferWindowHardware(int enabled)
{
    InterlockedExchange(&s_fb_hardware, enabled != 0);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];
static volatile LONG s_key_pending[256];
static volatile LONG s_mouse_dx, s_mouse_dy;
static BOOL s_mouse_captured, s_fullscreen;
static WINDOWPLACEMENT s_window_placement = { sizeof(WINDOWPLACEMENT) };

static int fb_option(const char *name, int fallback)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

void xbox_FramebufferMouseDelta(int *dx, int *dy)
{
    *dx = (int)InterlockedExchange(&s_mouse_dx, 0);
    *dy = (int)InterlockedExchange(&s_mouse_dy, 0);
}

static void fb_clip_mouse(HWND hwnd)
{
    RECT client;
    if (!s_mouse_captured || !IsWindowVisible(hwnd) || GetForegroundWindow()!=hwnd) return;
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, NULL, (POINT *)&client, 2);
    ClipCursor(&client);
}

/* Something else (Ctrl+Alt+Del, an overlay calling ClipCursor(NULL), a display
 * change) can clear the clip while the mouse is captured, and the hidden cursor
 * then drifts onto another monitor. Cheap enough to check a few times a second:
 * re-apply only when the system clip no longer equals the client rectangle. */
static void fb_reapply_clip(HWND hwnd)
{
    RECT client, current;
    if (!s_mouse_captured || !IsWindowVisible(hwnd) || GetForegroundWindow()!=hwnd) return;
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, NULL, (POINT *)&client, 2);
    if (GetClipCursor(&current) && EqualRect(&current, &client)) return;
    fb_clip_mouse(hwnd);
}

static void fb_capture_mouse(HWND hwnd, BOOL capture)
{
    if (capture && (!pc_setting_get(PC_MOUSE_AIM, fb_option("RECOMP_MOUSE_AIM", 0)) ||
                    pc_frontend_captured())) return;
    if (s_mouse_captured == capture) return;
    s_mouse_captured = capture;
    InterlockedExchange(&s_mouse_dx, 0);
    InterlockedExchange(&s_mouse_dy, 0);
    /* Owned hidden probes exercise input without taking the desktop cursor. */
    if(!IsWindowVisible(hwnd))return;
    if (capture) {
        SetCapture(hwnd);
        fb_clip_mouse(hwnd);
        SetCursor(NULL);
    } else {
        ClipCursor(NULL);
        ReleaseCapture();
        SetCursor(LoadCursorA(NULL, IDC_ARROW));
    }
}

static void fb_toggle_fullscreen(HWND hwnd)
{
    BOOL captured = s_mouse_captured;
    LONG_PTR visibility = GetWindowLongPtrA(hwnd,GWL_STYLE) & WS_VISIBLE;
    fb_capture_mouse(hwnd, FALSE);
    s_fullscreen = !s_fullscreen;
    if (s_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        GetWindowPlacement(hwnd, &s_window_placement);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrA(hwnd, GWL_STYLE, WS_POPUP | visibility);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | (visibility?0:SWP_NOACTIVATE));
    } else {
        WINDOWPLACEMENT placement=s_window_placement;
        if(!visibility)placement.showCmd=SW_HIDE;
        SetWindowLongPtrA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visibility);
        SetWindowPlacement(hwnd, &placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (captured) fb_capture_mouse(hwnd, TRUE);
    if(getenv("RECOMP_DISPLAY_TRACE")) {
        RECT r={};GetClientRect(hwnd,&r);
        fprintf(stderr,"[PC-DISPLAY] fullscreen=%d visible=%d client=%ldx%ld\n",
            s_fullscreen,IsWindowVisible(hwnd)!=0,r.right,r.bottom);
    }
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* Frames presented so far; the hang monitor (main.c, W-07) watches it move. */
LONG xbox_FramebufferFlipCount(void) { return s_flips; }

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    {
        static int latch = -1;
        if (latch < 0) latch = fb_option("RECOMP_INPUT_LATCH", 0);
        return s_key_down[vk] != 0 || (latch && s_key_pending[vk] != 0);
    }
}

void xbox_FramebufferAcknowledgeKey(int vk)
{
    if ((unsigned)vk < 256) InterlockedExchange(&s_key_pending[vk], 0);
}

static void fb_fit_to_work_area(HWND hwnd, LONG client_w, LONG client_h);

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if(m==PC_WINDOW_MOUSE_MESSAGE && (getenv("RECOMP_UI_SCRIPT") || getenv("RECOMP_UI_LIVE"))) {
        if(s_mouse_captured && !pc_frontend_captured() && pc_setting_get(PC_MOUSE_AIM,1)) {
            InterlockedAdd(&s_mouse_dx,(LONG)(intptr_t)w);InterlockedAdd(&s_mouse_dy,(LONG)l);
        }
        return 0;
    }
    if (m == PC_WINDOW_CAPTURE_MESSAGE) {
        fb_capture_mouse(h, FALSE);
        pc_input_camera_capture(0);
        memset((void *)s_key_down, 0, sizeof s_key_down);
        for (int key=0; key<256; key++) InterlockedExchange(&s_key_pending[key],0);
        return 0;
    }
    if(m==PC_WINDOW_CAMERA_MESSAGE){fb_capture_mouse(h,w!=0);pc_input_camera_capture(w!=0);return 0;}
    if(m==PC_WINDOW_AIM_MESSAGE) {
        if(pc_input_context()==PC_INPUT_GAMEPLAY && !pc_display_movie_active() &&
           (GetForegroundWindow()==h || !IsWindowVisible(h)))fb_capture_mouse(h, TRUE);
        return 0;
    }
    if (m == PC_WINDOW_CONFIG_MESSAGE) {
        BOOL fullscreen = w != 0;
        if(l)pc_render_size(LOWORD(l),HIWORD(l));
        if (s_fullscreen != fullscreen) fb_toggle_fullscreen(h);
        if (!fullscreen && l) {
            RECT size={0,0,LOWORD(l),HIWORD(l)};
            AdjustWindowRectExForDpi(&size,WS_OVERLAPPEDWINDOW,FALSE,0,GetDpiForWindow(h));
            SetWindowPos(h,NULL,0,0,size.right-size.left,size.bottom-size.top,
                         SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            fb_fit_to_work_area(h,LOWORD(l),HIWORD(l));
        }
        return 0;
    }
    if(m==WM_MOUSEMOVE) {
        static int previous_x,previous_y;
        int x=(short)LOWORD(l),y=(short)HIWORD(l);
        if(abs(x-previous_x)+abs(y-previous_y)>=2) {
            previous_x=x;previous_y=y;pc_input_note_keyboard();
        }
    }
    if(m==WM_KEYDOWN || m==WM_LBUTTONDOWN || m==WM_RBUTTONDOWN ||
       m==WM_MBUTTONDOWN || m==WM_XBUTTONDOWN)pc_input_note_keyboard();
    /* W-10 (D-3): pause in play and mute while another window has focus. [Input] PauseOnFocusLoss /
     * RECOMP_PAUSE_ON_FOCUS_LOSS and [Audio] MuteInBackground / RECOMP_MUTE_IN_BACKGROUND, both default 1.
     * A hidden harness window never pauses (unless RECOMP_TEST_FOCUS_PAUSE), so sweeps are unaffected. */
    if(m==WM_KILLFOCUS) {
        if(pc_switch("RECOMP_MUTE_IN_BACKGROUND","Audio","MuteInBackground",1))mcpx_apu_set_background(1);
        if(pc_switch("RECOMP_PAUSE_ON_FOCUS_LOSS","Input","PauseOnFocusLoss",1) && !pc_display_movie_active() &&
           (IsWindowVisible(h) || getenv("RECOMP_TEST_FOCUS_PAUSE")))
            xbox_InputPauseTap("window lost focus");
    }
    if(m==WM_SETFOCUS)mcpx_apu_set_background(0);
    if(m==WM_SETFOCUS) {
        xbox_InputFeedbackFocus(TRUE);
        /* Alt+Tab back into a match: mouse look returns without a click. The
         * AIM handler checks gameplay, no movie and foreground. */
        if(pc_input_device()==PC_INPUT_KEYBOARD_MOUSE)PostMessageA(h,PC_WINDOW_AIM_MESSAGE,0,0);
    }
    if(m==WM_KILLFOCUS || m==WM_CLOSE || m==WM_DESTROY)xbox_InputFeedbackFocus(FALSE);
    if (pc_frontend_message(h,m,w,l))
        return m==WM_INPUT?DefWindowProcA(h,m,w,l):0;
    switch (m) {
    case WM_CLOSE:
        fb_capture_mouse(h, FALSE);
        if (fb_option("RECOMP_EXIT_ON_CLOSE", 0)) {
            LARGE_INTEGER now={0},frequency={0};
            QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
            fprintf(stderr,"[EXIT] phase=terminate source=fb_wndproc reason=WM_CLOSE pid=%lu tid=%lu qpc=%lld frequency=%lld message=0x%04X wparam=0x%llX lparam=0x%llX exit_code=0\n",
                GetCurrentProcessId(),GetCurrentThreadId(),(long long)now.QuadPart,(long long)frequency.QuadPart,
                m,(unsigned long long)w,(unsigned long long)l);
            fflush(stderr);
            ExitProcess(0);
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    /* A tap of Alt or F10 would start the system-menu loop, which eats the next
     * key-up: a held key then stays down for ever and letters turn into menu
     * mnemonics. Block only the keyboard-driven menu (lParam 0); Alt+F4,
     * Alt+Enter and Alt+Space (lParam is the space character) keep working. */
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0) return 0;
        break;

    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) fb_reapply_clip(h);
        break;

    /* A desktop resolution or monitor layout change leaves a borderless
     * window and the clip on the old rectangle. */
    case WM_DISPLAYCHANGE:
        if (s_fullscreen) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            fb_clip_mouse(h);
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (w == VK_F11 || (w == VK_RETURN && (l & (1L << 29)))) {
            if (!(l & (1L << 30))) fb_toggle_fullscreen(h);
            return 0;
        }
        if (w == VK_F8) {
            if (!(l & (1L << 30))) {
                fb_capture_mouse(h, !s_mouse_captured);
                pc_input_camera_capture(s_mouse_captured && pc_input_context()==PC_INPUT_MENU);
            }
            return 0;
        }
        if (w == VK_ESCAPE) fb_capture_mouse(h, FALSE);
        if ((unsigned)w < 256) {
            s_key_down[w] = 1;
            if (!(l & (1L << 30))) InterlockedExchange(&s_key_pending[w], 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        for (int key = 0; key < 256; key++) InterlockedExchange(&s_key_pending[key], 0);
        fb_capture_mouse(h, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
        SetFocus(h);
        const BOOL was_captured=s_mouse_captured;
        if(pc_input_context()==PC_INPUT_GAMEPLAY && !pc_display_movie_active())fb_capture_mouse(h, TRUE);
        /* The click that takes the mouse back is not a shot (it used to fire RT/LT too). */
        if(!was_captured && s_mouse_captured)return m==WM_XBUTTONDOWN?TRUE:0;
        int key=m==WM_LBUTTONDOWN?VK_LBUTTON:m==WM_RBUTTONDOWN?VK_RBUTTON:m==WM_MBUTTONDOWN?VK_MBUTTON:
            GET_XBUTTON_WPARAM(w)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2;
        s_key_down[key]=1;InterlockedExchange(&s_key_pending[key],1);
        return m==WM_XBUTTONDOWN?TRUE:0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
        int key=m==WM_LBUTTONUP?VK_LBUTTON:m==WM_RBUTTONUP?VK_RBUTTON:m==WM_MBUTTONUP?VK_MBUTTON:
            GET_XBUTTON_WPARAM(w)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2;
        s_key_down[key]=0;return m==WM_XBUTTONUP?TRUE:0;
    }
    case WM_CAPTURECHANGED:
        if (s_mouse_captured && (HWND)l != h) {fb_capture_mouse(h, FALSE);pc_input_camera_capture(0);}
        return 0;
    case WM_MOVE:
        fb_clip_mouse(h);
        break;
    case WM_SIZE:
        pc_display_size(LOWORD(l),HIWORD(l));
        /* Windows may constrain a window's client area, even after an explicit
         * resolution selection. Presentation follows that area; scene pixels
         * remain the selected resolution in either window mode. */
        fb_clip_mouse(h);
        break;
    case WM_GETMINMAXINFO: {
        RECT minimum={0,0,640,480};
        AdjustWindowRectExForDpi(&minimum,WS_OVERLAPPEDWINDOW,FALSE,0,GetDpiForWindow(h));
        MINMAXINFO *limits=(MINMAXINFO *)l;
        limits->ptMinTrackSize.x=minimum.right-minimum.left;
        limits->ptMinTrackSize.y=minimum.bottom-minimum.top;
        return 0;
    }
    case WM_DPICHANGED:
        if(!s_fullscreen) {
            const RECT *suggested=(const RECT *)l;
            SetWindowPos(h,NULL,suggested->left,suggested->top,
                suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        fb_clip_mouse(h);
        return 0;
    case WM_SETCURSOR:
        if (pc_frontend_captured() && LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            return TRUE;
        }
        if (s_mouse_captured && LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_INPUT:
        if (s_mouse_captured && pc_setting_get(PC_MOUSE_AIM,1)) {
            RAWINPUT input;
            UINT size = sizeof(input);
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &input, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                input.header.dwType == RIM_TYPEMOUSE &&
                !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedExchangeAdd(&s_mouse_dx, input.data.mouse.lLastX);
                InterlockedExchangeAdd(&s_mouse_dy, input.data.mouse.lLastY);
                if(input.data.mouse.lLastX || input.data.mouse.lLastY)pc_input_note_keyboard();
            }
        }
        /* DefWindowProc releases the foreground raw-input record. */
        return DefWindowProcA(h, m, w, l);
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* Probe commands only address the HWND owned by this game. They never inject
 * desktop input or depend on another application having focus. */
static void fb_ui_action(HWND hwnd,const char *action)
{
    static BOOL restore_hidden;
    if (!strcmp(action,"fullscreen"))
        PostMessageA(hwnd,WM_SYSKEYDOWN,VK_RETURN,(LPARAM)1<<29);
    else if (!strcmp(action,"close"))
        PostMessageA(hwnd,WM_CLOSE,0,0);
    else if (!strcmp(action,"minimize")) {
        restore_hidden=!IsWindowVisible(hwnd);
        ShowWindow(hwnd,restore_hidden?SW_SHOWMINNOACTIVE:SW_MINIMIZE);
    }
    else if (!strcmp(action,"restore")) {
        ShowWindow(hwnd,restore_hidden?SW_SHOWNOACTIVATE:SW_RESTORE);
        if(restore_hidden)ShowWindow(hwnd,SW_HIDE);
        restore_hidden=FALSE;
    }
    else if (!strcmp(action,"focus-lost"))PostMessageA(hwnd,WM_KILLFOCUS,0,0);
    else if (!strcmp(action,"focus-gain"))PostMessageA(hwnd,WM_SETFOCUS,0,0);
    /* The message an Alt or F10 tap makes DefWindowProc send. Unblocked, it starts the
     * system-menu loop, which eats the next key-up (a held key stays down). */
    else if (!strcmp(action,"sys-menu"))PostMessageA(hwnd,WM_SYSCOMMAND,SC_KEYMENU,0);
    else if (!strcmp(action,"window-status")) {
        RECT r={0};unsigned width,height;GetClientRect(hwnd,&r);pc_render_size_get(&width,&height);
        fprintf(stderr,"[PC-WINDOW] fullscreen=%d visible=%d minimized=%d client=%ldx%ld render=%ux%u\n",
            s_fullscreen,IsWindowVisible(hwnd)!=0,IsIconic(hwnd)!=0,r.right,r.bottom,width,height);
    }
    else if (!strcmp(action,"input-status")) {
        unsigned held=0,edges=0;
        for(unsigned key=0;key<256;key++) {held+=s_key_down[key]!=0;edges+=s_key_pending[key]!=0;}
        fprintf(stderr,"[PC-UI] input capture=%d panel=%d keyboard=%d mouse=%d pending=%ld,%ld focus=%d held=%u edges=%u\n",
            s_mouse_captured,pc_frontend_captured(),pc_setting_get(PC_KEYBOARD,1),
            pc_setting_get(PC_MOUSE_AIM,1),s_mouse_dx,s_mouse_dy,GetFocus()==hwnd,held,edges);
    }
    else if(!strcmp(action,"input-device=gamepad")){
        pc_input_note_gamepad(0,0,0,0,0,0,0);pc_input_note_gamepad(1,0,0,0,0,0,0);
    }
    else if(!strcmp(action,"input-device=keyboard"))pc_input_note_keyboard();
    else if (!strncmp(action,"resize=",7)) {
        unsigned width,height;
        if(sscanf(action+7,"%ux%u",&width,&height)==2 && width>=320 &&
           width<=7680 && height>=240 && height<=4320)
            SetWindowPos(hwnd,NULL,0,0,(int)width,(int)height,SWP_NOMOVE|SWP_NOZORDER);
    }
    else if (!strncmp(action,"key=",4)) {
        char *ending;
        unsigned key=(unsigned)strtoul(action+4,&ending,0);
        if(key>0 && key<256) {
            if (!strcmp(ending,"down")) PostMessageA(hwnd,WM_KEYDOWN,key,1);
            else if (!strcmp(ending,"up")) PostMessageA(hwnd,WM_KEYUP,key,(LPARAM)3<<30);
            else if (!*ending) {
                PostMessageA(hwnd,WM_KEYDOWN,key,1);
                PostMessageA(hwnd,WM_KEYUP,key,(LPARAM)3<<30);
            }
        }
    }
    else if (!strncmp(action,"wheel=",6)) {
        int steps=atoi(action+6);
        if(steps>=-20 && steps<=20)
            PostMessageA(hwnd,WM_MOUSEWHEEL,MAKEWPARAM(0,steps*WHEEL_DELTA),0);
    }
    else if (!strncmp(action,"mouse=",6)) {
        int mx,my;
        if(sscanf(action+6,"%dx%d",&mx,&my)==2 && mx>=-10000 && mx<=10000 && my>=-10000 && my<=10000)
            PostMessageA(hwnd,PC_WINDOW_MOUSE_MESSAGE,(WPARAM)(intptr_t)mx,(LPARAM)my);
    }
    else if (!strncmp(action,"drag=",5)) {
        unsigned x0,y0,x1,y1;
        if(sscanf(action+5,"%ux%u:%ux%u",&x0,&y0,&x1,&y1)==4 &&
           x0<7680 && x1<7680 && y0<4320 && y1<4320) {
            PostMessageA(hwnd,WM_MOUSEMOVE,0,MAKELPARAM(x0,y0));
            PostMessageA(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x0,y0));
            for(int i=1;i<=8;i++)PostMessageA(hwnd,WM_MOUSEMOVE,MK_LBUTTON,
                MAKELPARAM((int)x0+((int)x1-(int)x0)*i/8,(int)y0+((int)y1-(int)y0)*i/8));
            PostMessageA(hwnd,WM_LBUTTONUP,0,MAKELPARAM(x1,y1));
        }
    }
    else if (!strncmp(action,"click=",6) || !strncmp(action,"hover=",6)) {
        unsigned mx,my;
        if(sscanf(action+6,"%ux%u",&mx,&my)==2 && mx<7680 && my<4320) {
            LPARAM position=MAKELPARAM(mx,my);
            PostMessageA(hwnd,WM_MOUSEMOVE,0,position);
            if(action[0]=='c') {
                PostMessageA(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,position);
                PostMessageA(hwnd,WM_LBUTTONUP,0,position);
            }
        }
    }
}

/* A window larger than its monitor's work area (RECOMP_WINDOW_WIDTH/HEIGHT or
 * a saved size from a bigger screen) has its title bar and edges off-screen.
 * Scale the client area down by the smaller ratio, keep the aspect, centre it.
 * The render size is unchanged: only the window shrinks. */
static void fb_fit_to_work_area(HWND hwnd, LONG client_w, LONG client_h)
{
    MONITORINFO mi = { sizeof(mi) };
    RECT outer, nc = {0, 0, client_w, client_h};
    UINT dpi = GetDpiForWindow(hwnd);
    LONG nc_w, nc_h, work_w, work_h, new_w, new_h;
    double scale, sx, sy;
    if (s_fullscreen || client_w <= 0 || client_h <= 0) return;
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
    if (!GetWindowRect(hwnd, &outer)) return;
    AdjustWindowRectExForDpi(&nc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi ? dpi : 96);
    nc_w = (nc.right - nc.left) - client_w;
    nc_h = (nc.bottom - nc.top) - client_h;
    work_w = mi.rcWork.right - mi.rcWork.left;
    work_h = mi.rcWork.bottom - mi.rcWork.top;
    if ((outer.right - outer.left) <= work_w && (outer.bottom - outer.top) <= work_h) return;
    sx = (double)(work_w - nc_w) / client_w;
    sy = (double)(work_h - nc_h) / client_h;
    scale = sx < sy ? sx : sy;
    if (scale >= 1.0 || scale <= 0.0) return;
    new_w = (LONG)(client_w * scale);
    new_h = (LONG)(client_h * scale);
    if (new_w < 320 || new_h < 240) return;
    SetWindowPos(hwnd, NULL, mi.rcWork.left + (work_w - (new_w + nc_w)) / 2,
                 mi.rcWork.top + (work_h - (new_h + nc_h)) / 2, new_w + nc_w, new_h + nc_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    fprintf(stderr, "[FBWIN] window %ldx%ld exceeds the work area %ldx%ld: shrunk to %ldx%ld (render size unchanged)\n",
            client_w, client_h, work_w, work_h, new_w, new_h);
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.hIcon         = LoadIconA(wc.hInstance,MAKEINTRESOURCEA(101));
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    r.left = 0; r.top = 0;
    r.right = fb_option("RECOMP_WINDOW_WIDTH", (int)s_fb_width);
    r.bottom = fb_option("RECOMP_WINDOW_HEIGHT", (int)s_fb_height);
    if (r.right < 320 || r.right > 7680) r.right = (LONG)s_fb_width;
    if (r.bottom < 240 || r.bottom > 4320) r.bottom = (LONG)s_fb_height;
    const LONG requested_width=r.right,requested_height=r.bottom;
    UINT initial_dpi=GetDpiForSystem();
    if(!initial_dpi)initial_dpi=96;
    AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,initial_dpi);
    const BOOL diagnostic_hidden=fb_option("RECOMP_FB_HIDDEN",0)!=0;
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | (diagnostic_hidden ? 0 : WS_VISIBLE),
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    InterlockedExchangePointer(&s_fb_window, hwnd);
    /* CW_USEDEFAULT can place the window on a monitor with another DPI. */
    UINT window_dpi=GetDpiForWindow(hwnd);
    if(window_dpi && window_dpi!=initial_dpi) {
        RECT size={0,0,requested_width,requested_height};
        AdjustWindowRectExForDpi(&size,WS_OVERLAPPEDWINDOW,FALSE,0,window_dpi);
        SetWindowPos(hwnd,NULL,0,0,size.right-size.left,size.bottom-size.top,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    fb_fit_to_work_area(hwnd, requested_width, requested_height);
    {RECT client;GetClientRect(hwnd,&client);pc_display_size(client.right,client.bottom);}
    hdc = GetDC(hwnd);
    {
        RAWINPUTDEVICE mouse = { 1, 2, 0, hwnd };
        if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
            fprintf(stderr, "  [FBWIN] raw mouse registration failed: %lu\n", GetLastError());
    }
    if (fb_option("RECOMP_FULLSCREEN", 0)) fb_toggle_fullscreen(hwnd);
    SetStretchBltMode(hdc, fb_option("RECOMP_SCALE_FILTER", 1) ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(hdc, 0, 0, NULL);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    /* Diagnostic actions execute through this game's own window handler,
     * without injecting input into another application. */
    const char *ui_script = getenv("RECOMP_UI_SCRIPT");
    const char *ui_live = getenv("RECOMP_UI_LIVE");
    long ui_live_offset=0;
    DWORD ui_live_poll=0;
    DWORD ui_started = GetTickCount();
    /* The environment is fixed once main.c has started; the old per-pass getenv
     * calls took the CRT environment lock up to 8000 times a second. */
    const int preserve_aspect = fb_option("RECOMP_PRESERVE_ASPECT", 1);
    const char *fb_dump_path = getenv("RECOMP_FB_DUMP");
    const char *fb_dump_every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
    const int fb_dump_period = fb_dump_every ? atoi(fb_dump_every) : 0;
    DWORD clip_check = 0;
    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        RECT client;
        int x = 0, y = 0, width, height;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* A system minimize/restore transition can finish after ShowWindow
         * returns. Keep explicitly hidden diagnostic runs hidden once the
         * window is restored; normal launches do not enter this path. */
        if(diagnostic_hidden && !IsIconic(hwnd) && IsWindowVisible(hwnd))
            ShowWindow(hwnd,SW_HIDE);
        while (ui_script && *ui_script) {
            unsigned at = 0, consumed = 0;
            char action[64] = {0};
            if (sscanf(ui_script, "%u:%63[^,]%n", &at, action, &consumed) < 2 || !consumed) {
                fprintf(stderr, "[PC-UI] Invalid diagnostic action: %s\n", ui_script);
                ui_script = NULL;
                break;
            }
            if (GetTickCount() - ui_started < at) break;
            fprintf(stderr, "[PC-UI] t=%lu action=%s console=%s\n", GetTickCount()-ui_started,
                action, GetConsoleWindow() ? "present" : "absent");
            fb_ui_action(hwnd,action);
            ui_script += consumed;
            if (*ui_script == ',') ++ui_script;
        }
        if(ui_live && *ui_live && GetTickCount()-ui_live_poll>=50) {
            ui_live_poll=GetTickCount();
            FILE *commands=fopen(ui_live,"rb");
            if(commands) {
                if(!fseek(commands,ui_live_offset,SEEK_SET)) {
                    char action[128];
                    while(fgets(action,sizeof(action),commands)) {
                        char *newline=strchr(action,'\n');
                        if(!newline)break; /* The writer has not completed this command yet. */
                        ui_live_offset=ftell(commands);
                        *newline=0;size_t length=strlen(action);
                        if(length && action[length-1]=='\r')action[--length]=0;
                        if(length) {
                            fprintf(stderr,"[PC-UI] t=%lu live=%s console=%s\n",GetTickCount()-ui_started,
                                action,GetConsoleWindow()?"present":"absent");
                            fb_ui_action(hwnd,action);
                        }
                    }
                }
                fclose(commands);
            }
        }
        if (!s_fb_running) break;
        GetClientRect(hwnd, &client);
        width = client.right;
        height = client.bottom;
        if (preserve_aspect && width > 0 && height > 0) {
            if ((long long)width * s_fb_height > (long long)height * s_fb_width) {
                width = (int)((long long)height * s_fb_width / s_fb_height);
                x = (client.right - width) / 2;
            } else {
                height = (int)((long long)width * s_fb_height / s_fb_width);
                y = (client.bottom - height) / 2;
            }
        }
        if (!s_fb_hardware && (x || y)) {
            RECT bar = client;
            if (x) {
                bar.right = x; FillRect(hdc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
                bar.left = x + width; bar.right = client.right;
            } else {
                bar.bottom = y; FillRect(hdc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
                bar.top = y + height; bar.bottom = client.bottom;
            }
            FillRect(hdc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        if (!s_fb_hardware && s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            StretchDIBits(hdc, x, y, width, height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else if (!s_fb_hardware && s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            StretchDIBits(hdc, x, y, width, height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = fb_dump_path;
            static int frames;
            int period = fb_dump_period;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                if(pc_setting_get(PC_SHOW_FPS,fb_option("RECOMP_SHOW_FPS",0)))
                    _snwprintf(tb,127,L"%ls | %.1f FPS",s_title,t0?(f-f0)*1000.0/(now-t0):0.0);
                else _snwprintf(tb,127,L"%ls",s_title);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        /* Re-apply the cursor clip if something else cleared it (see fb_reapply_clip). */
        if (s_mouse_captured) {
            DWORD tick = GetTickCount();
            if (tick - clip_check >= 250) {clip_check = tick; fb_reapply_clip(hwnd);}
        }
        /* Sleep(4) also delays WM_INPUT that arrives during the sleep,
         * batching a high-polling-rate mouse into roughly 250 Hz updates.
         * Wake on queued input immediately; retain the timeout for window
         * maintenance, software presentation and diagnostic commands.
         * INPUTAVAILABLE also catches messages posted by fb_ui_action above. */
        MsgWaitForMultipleObjectsEx(0, NULL, 4, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    InterlockedExchangePointer(&s_fb_window, NULL);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif
