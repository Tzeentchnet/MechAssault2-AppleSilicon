/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "../platform/pc_frontend.h"
#include "../platform/pc_switch.h"
#include "pc_stick.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#include "xinput_feedback.h"
#include "xinput_test_shim.h"
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* ---- host pad access, and the active slot --------------------------------
 *
 * Every host call goes through xi_get / xi_set / xi_caps so the test-only fake
 * XInput (xinput_test_shim.h, env-gated, off in every normal run) can stand in
 * for the real one.
 *
 * The guest has one virtual pad (guest port 0). It used to read host slot 0 only,
 * so a pad in slot 1-3 (a second pad, DS4Windows / ViGEm / Steam Input holding
 * slot 0, a dead battery in the first pad) did nothing. xi_poll_active picks the
 * host slot that drives guest port 0:
 *   - slot 0 if it is connected, otherwise the lowest connected slot;
 *   - it moves to another connected slot when that slot shows a NEW button or
 *     trigger press (never a stick move, so a drifting pad cannot take over);
 *   - when the active pad disappears it falls back to the lowest connected slot.
 * An empty slot is probed at most once a second (XInput re-enumerates devices for
 * an empty slot, which is costly at the 250 polls a second the USB tick makes). A
 * connected slot is polled on every call. */
#define XI_PROBE_MS 1000
#define XI_TRIGGER_PRESS 30
typedef struct {
    BOOL connected;
    BOOL probed;            /* next_probe is valid */
    DWORD next_probe;       /* GetTickCount deadline for the next probe of an empty slot */
    WORD prev_buttons;
    BYTE prev_lt, prev_rt;
} XiSlot;
static XiSlot xi_slot[4];
static SRWLOCK xi_lock = SRWLOCK_INIT;
static volatile LONG xi_active_slot = 0;
static volatile LONG xi_switch_epoch = 0;

static DWORD xi_get(DWORD slot, XINPUT_STATE *state)
{
    if (xs_enabled()) return xs_get_state(slot, state);
    return XInputGetState(slot, state);
}
static DWORD WINAPI xi_set(DWORD slot, XINPUT_VIBRATION *vibration)
{
    if (xs_enabled()) return xs_set_state(slot, vibration);
    return XInputSetState(slot, vibration);
}
static DWORD xi_caps(DWORD slot, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    if (xs_enabled()) return xs_get_caps(slot, flags, caps);
    return XInputGetCapabilities(slot, flags, caps);
}

DWORD xbox_InputActiveSlot(void)
{
    return (DWORD)InterlockedCompareExchange(&xi_active_slot, 0, 0);
}

/* Guest port 0 is the active host slot; ports 1-3 keep their own host slots. */
static DWORD xi_host_slot(DWORD guest_port)
{
    return guest_port == 0 ? xbox_InputActiveSlot() : guest_port;
}
static DWORD WINAPI xi_send_mapped(DWORD guest_port, XINPUT_VIBRATION *vibration)
{
    return xi_set(xi_host_slot(guest_port), vibration);
}

/* Returns ERROR_SUCCESS and the active slot's state, or ERROR_DEVICE_NOT_CONNECTED
 * when no pad is connected. *lost is set when the active pad was connected on the
 * previous poll and is gone now (the caller pauses the game). */
static DWORD xi_poll_active(XINPUT_STATE *out, BOOL *lost)
{
    XINPUT_STATE state[4];
    BOOL fresh[4] = { FALSE, FALSE, FALSE, FALSE };
    DWORD now = GetTickCount(), previous_active, active;
    BOOL was_connected, stop_old;
    DWORD result;
    int i;

    if (lost) *lost = FALSE;
    AcquireSRWLockExclusive(&xi_lock);
    previous_active = active = (DWORD)xi_active_slot;
    was_connected = xi_slot[active].connected;
    for (i = 0; i < 4; i++) {
        XiSlot *s = &xi_slot[i];
        if (!s->connected && s->probed && (LONG)(now - s->next_probe) < 0) continue;
        if (xi_get((DWORD)i, &state[i]) == ERROR_SUCCESS) {
            if (!s->connected) {
                /* A pad that just appeared has no "new press": remember what it already holds. */
                s->connected = TRUE;
                s->prev_buttons = state[i].Gamepad.wButtons;
                s->prev_lt = state[i].Gamepad.bLeftTrigger;
                s->prev_rt = state[i].Gamepad.bRightTrigger;
                fprintf(stderr, "[PC-INPUT] controller connected in slot %d\n", i);
            }
            fresh[i] = TRUE;
        } else {
            if (s->connected) fprintf(stderr, "[PC-INPUT] controller disconnected from slot %d\n", i);
            s->connected = FALSE;
            s->probed = TRUE;
            s->next_probe = now + XI_PROBE_MS;
        }
    }
    if (!xi_slot[active].connected) {
        for (i = 0; i < 4; i++) if (xi_slot[i].connected) { active = (DWORD)i; break; }
    } else {
        for (i = 0; i < 4; i++) {
            const XINPUT_GAMEPAD *g;
            if ((DWORD)i == active || !xi_slot[i].connected || !fresh[i]) continue;
            g = &state[i].Gamepad;
            if ((g->wButtons & ~xi_slot[i].prev_buttons) ||
                (g->bLeftTrigger > XI_TRIGGER_PRESS && xi_slot[i].prev_lt <= XI_TRIGGER_PRESS) ||
                (g->bRightTrigger > XI_TRIGGER_PRESS && xi_slot[i].prev_rt <= XI_TRIGGER_PRESS)) {
                active = (DWORD)i;
                break;
            }
        }
    }
    for (i = 0; i < 4; i++) if (fresh[i]) {
        xi_slot[i].prev_buttons = state[i].Gamepad.wButtons;
        xi_slot[i].prev_lt = state[i].Gamepad.bLeftTrigger;
        xi_slot[i].prev_rt = state[i].Gamepad.bRightTrigger;
    }
    if (active != previous_active) {
        InterlockedExchange(&xi_active_slot, (LONG)active);
        InterlockedIncrement(&xi_switch_epoch);
        fprintf(stderr, "[PC-INPUT] active controller slot %lu -> %lu (%s)\n",
                (unsigned long)previous_active, (unsigned long)active,
                xi_slot[previous_active].connected ? "button pressed" : "previous pad gone");
    }
    stop_old = active != previous_active && xi_slot[previous_active].connected;
    if (lost && was_connected && !xi_slot[previous_active].connected) *lost = TRUE;
    if (xi_slot[active].connected) { *out = state[active]; result = ERROR_SUCCESS; }
    else result = ERROR_DEVICE_NOT_CONNECTED;
    ReleaseSRWLockExclusive(&xi_lock);
    if (stop_old) {
        /* Stop the old pad's motors; the guest's next effect goes to the new slot. */
        XINPUT_VIBRATION stopped = { 0, 0 };
        xi_set(previous_active, &stopped);
    }
    return result;
}

/* For the native settings page and the Editor button: the active pad's raw state. */
DWORD xbox_InputPollHostState(XINPUT_STATE *state)
{
    return state ? xi_poll_active(state, NULL) : ERROR_DEVICE_NOT_CONNECTED;
}

/* True when any host slot holds a pad (empty slots are probed at most once a second). */
BOOL xbox_InputAnyPadConnected(void)
{
    XINPUT_STATE state;
    return xi_poll_active(&state, NULL) == ERROR_SUCCESS;
}

/* ---- pause when the active pad is unplugged ------------------------------
 *
 * The virtual USB pad never detaches, so the title's own "controller removed"
 * handling never runs and a dead battery in the middle of a match just leaves the
 * 'Mech standing there. When the active pad goes away while a gamepad is the
 * input device and the game is in play, hold START for a moment: the title pauses
 * exactly as if the player had pressed it. */
#define XI_PAUSE_TAP_MS 200
static volatile LONGLONG xi_pause_tap_until;
static volatile LONG xi_pause_taps;
static ULONGLONG xi_tick64(void) { return GetTickCount64(); }

static void xi_arm_pause_tap(void)
{
    InterlockedExchange64(&xi_pause_tap_until, (LONGLONG)(xi_tick64() + XI_PAUSE_TAP_MS));
    LONG n = InterlockedIncrement(&xi_pause_taps);
    fprintf(stderr, "[PC-INPUT] active controller lost during play: START tap #%ld to pause\n", (long)n);
}

/* W-10: the window lost focus while a match or mission is in play: pause it the same way (START tap).
 * Menus, movies and the title are left alone. */
void xbox_InputPauseTap(const char *why)
{
    if (pc_input_context() != PC_INPUT_GAMEPLAY || pc_display_movie_active()) return;
    InterlockedExchange64(&xi_pause_tap_until, (LONGLONG)(xi_tick64() + XI_PAUSE_TAP_MS));
    LONG n = InterlockedIncrement(&xi_pause_taps);
    fprintf(stderr, "[PC-INPUT] %s during play: START tap #%ld to pause\n", why ? why : "pause", (long)n);
}

static DWORD xi_apply_pause_tap(DWORD result, XBOX_INPUT_STATE *state)
{
    if ((ULONGLONG)InterlockedCompareExchange64(&xi_pause_tap_until, 0, 0) <= xi_tick64()) return result;
    if (result != ERROR_SUCCESS) memset(state, 0, sizeof *state);
    state->Gamepad.wButtons |= XBOX_GAMEPAD_START;
    state->dwPacketNumber += 0x40000000u;
    return ERROR_SUCCESS;
}
/* Four-port guest pause/drain gate. Zero at process start. */
static volatile LONG guest_pause_output_mask;
static unsigned feedback_output_ports(void)
{
    if(pc_input_context()!=PC_INPUT_GAMEPLAY || pc_frontend_captured())return 0;
    /* Other players retain their controller output when player one uses a
     * mouse. Pause/capture and focus still suppress every player's motors. */
    return (14u | (pc_input_device()==PC_INPUT_GAMEPAD?1u:0u)) &
        ~(unsigned)InterlockedCompareExchange(&guest_pause_output_mask,0,0);
}

/* Private opt-in evidence; guest input reads already occurred. The packed
 * snapshot is diagnostic-only and is never consumed by game input logic. */
static volatile LONG input_trace_enabled=-1,rumble_trace_enabled=-1;
static volatile LONG64 input_trace_state;
static SRWLOCK input_trace_lock=SRWLOCK_INIT;
static unsigned input_trace_epoch,input_trace_records;
static int input_trace_initial;
static int input_trace_option(volatile LONG *cached,const char *name)
{
    LONG value=InterlockedCompareExchange(cached,-1,-1);
    if(value<0) {
        const char *option=getenv(name);
        value=option && !strcmp(option,"1");
        InterlockedCompareExchange(cached,value,-1);
    }
    return value>0;
}
static int input_trace_active(void)
{
    LONG value=InterlockedCompareExchange(&input_trace_enabled,-1,-1);
    if(value<0) {
        const char *a=getenv("RECOMP_RUMBLE_TRACE"),*b=getenv("RECOMP_EFFECT_TRACE"),*c=getenv("RECOMP_UPLOAD_TRACE");
        value=(a&&!strcmp(a,"1"))||(b&&!strcmp(b,"1"))||(c&&!strcmp(c,"1"));
        InterlockedCompareExchange(&input_trace_enabled,value,-1);
    }
    return value>0;
}
unsigned long long xbox_InputTraceState(void)
{
    return (unsigned long long)InterlockedCompareExchange64(&input_trace_state,0,0);
}
static void input_trace_observe(DWORD port,const XBOX_INPUT_STATE *input)
{
    if(port || !input_trace_active())return;
    const XBOX_GAMEPAD *g=&input->Gamepad;
    unsigned mask=(g->bAnalogButtons[XBOX_BUTTON_RTRIGGER]>XBOX_ANALOG_BUTTON_THRESHOLD?1u:0u)|
        (g->bAnalogButtons[XBOX_BUTTON_LTRIGGER]>XBOX_ANALOG_BUTTON_THRESHOLD?2u:0u)|
        (g->wButtons&XBOX_GAMEPAD_LEFT_THUMB?4u:0u)|
        ((g->sThumbLX||g->sThumbLY)?8u:0u)|(g->wButtons&XBOX_GAMEPAD_RIGHT_THUMB?16u:0u);
    unsigned context=pc_input_context(),device=pc_input_device(),captured=pc_frontend_captured()!=0;
    unsigned body=mask|(context<<8)|(device<<10)|(captured<<11);
    AcquireSRWLockExclusive(&input_trace_lock);
    if(!input_trace_initial || (unsigned)xbox_InputTraceState()!=body) {
        input_trace_initial=1;input_trace_epoch++;
        InterlockedExchange64(&input_trace_state,((LONG64)input_trace_epoch<<32)|body);
        if(input_trace_records<128) {
            input_trace_records++;
            fprintf(stderr,"[PERF] input-action epoch=%u host_ms=%llu port=0 mask=%X rt=%u lt=%u left_click=%u stick_nonzero=%u context=%u device=%u captured=%u scope=existing_guest_input\n",
                input_trace_epoch,(unsigned long long)GetTickCount64(),mask,
                g->bAnalogButtons[XBOX_BUTTON_RTRIGGER],g->bAnalogButtons[XBOX_BUTTON_LTRIGGER],
                (mask&4)!=0,(mask&8)!=0,context,device,captured);
        }
    }
    ReleaseSRWLockExclusive(&input_trace_lock);
}
static void feedback_trace(const char *reason,DWORD port,XINPUT_VIBRATION requested,
    XINPUT_VIBRATION sent,DWORD result,int focused,unsigned allowed)
{
    if(!input_trace_option(&rumble_trace_enabled,"RECOMP_RUMBLE_TRACE"))return;
    /* Called under the existing feedback lock. The bounded record reflects
     * request/output order, including the original stop dispatch result. */
    typedef struct { XINPUT_VIBRATION requested,sent;DWORD result;int focused;unsigned allowed,body,duplicates;const char *reason;int valid,guest_seen;unsigned long long last_guest_request_ms; } Last;
    static Last last[4];static unsigned records;
    if(port>=4)return;
    unsigned long long input=xbox_InputTraceState();unsigned body=(unsigned)input;
    Last *previous=&last[port];
    unsigned long long now=(unsigned long long)GetTickCount64();
    unsigned long long previous_guest_ms=previous->last_guest_request_ms;
    int previous_guest_seen=previous->guest_seen;
    if(!strcmp(reason,"guest_output")) {
        previous->last_guest_request_ms=now;previous->guest_seen=1;
    }
    if(previous->valid && previous->requested.wLeftMotorSpeed==requested.wLeftMotorSpeed && previous->requested.wRightMotorSpeed==requested.wRightMotorSpeed &&
       previous->sent.wLeftMotorSpeed==sent.wLeftMotorSpeed && previous->sent.wRightMotorSpeed==sent.wRightMotorSpeed &&
       previous->result==result && previous->focused==focused && previous->allowed==allowed && previous->body==body && !strcmp(previous->reason,reason)) {
        previous->duplicates++;return;
    }
    if(records<256) {
        records++;
        fprintf(stderr,"[PERF] motor-output record=%u host_ms=%llu source=%s port=%lu request=%u/%u sent=%u/%u previous_sent=%u/%u duplicates=%u result=%lu focus=%d allowed=%X input_epoch=%u action=%X context=%u device=%u captured=%u previous_last_guest_request_ms=%llu guest_gap_ms=%llu previous_guest_request_known=%u scope=host_request_not_physical_motor\n",
            records,now,reason,(unsigned long)port,
            requested.wLeftMotorSpeed,requested.wRightMotorSpeed,sent.wLeftMotorSpeed,sent.wRightMotorSpeed,
            previous->valid?previous->sent.wLeftMotorSpeed:0,previous->valid?previous->sent.wRightMotorSpeed:0,previous->duplicates,
            (unsigned long)result,focused,allowed,(unsigned)(input>>32),body&31u,
            (unsigned)pc_input_context(),(unsigned)pc_input_device(),pc_frontend_captured()!=0,
            previous_guest_ms,previous_guest_seen && now>=previous_guest_ms?now-previous_guest_ms:0,previous_guest_seen!=0);
    }
    previous->requested=requested;previous->sent=sent;previous->result=result;previous->focused=focused;
    previous->allowed=allowed;previous->body=body;previous->reason=reason;previous->duplicates=0;previous->valid=1;
}

static XinputFeedback g_feedback={SRWLOCK_INIT,1,{0},feedback_output_ports,feedback_trace};
static void feedback_output_policy_changed(void)
{
    xinput_feedback_policy_changed(&g_feedback,xi_send_mapped);
}
static INIT_ONCE feedback_policy_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK register_feedback_policy(PINIT_ONCE once,PVOID parameter,PVOID *context)
{
    (void)once;(void)parameter;(void)context;
    pc_input_register_output_policy_callback(feedback_output_policy_changed);
    return TRUE;
}
static void ensure_feedback_policy(void)
{
    InitOnceExecuteOnce(&feedback_policy_once,register_feedback_policy,NULL,NULL);
}

void xbox_InputFeedbackPaused(unsigned blocked_ports)
{
    blocked_ports &= 15u;
    ensure_feedback_policy();
    if ((unsigned)InterlockedExchange(&guest_pause_output_mask,(LONG)blocked_ports) != blocked_ports)
        feedback_output_policy_changed();
}

void xbox_InputFeedbackFocus(BOOL focused)
{
    xinput_feedback_focus(&g_feedback,focused,xi_send_mapped);
}

#endif /* _WIN32: XInput-specific frontend */

/* ---- keyboard, when there is no pad --------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there, so a real controller keeps working.
 *
 * The keys are the ones a Dreamcast or Saturn emulator would pick, which is
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return pc_setting_get(PC_KEYBOARD, on) ? TRUE : FALSE;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);
extern void xbox_FramebufferMouseDelta(int *dx, int *dy);
extern void xbox_FramebufferAcknowledgeKey(int vk);

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* A thumb axis from two keys, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (key_down(negative)) v -= 32767;
    if (key_down(positive)) v += 32767;
    return (SHORT)v;
}

/* FT-RT (R2 P3-1): the left stick from four keys. Two axes at full key deflection are (+-32767, +-32767),
 * magnitude 1.414, which no pad stick can reach; with [Input] DiagonalNormalise (env RECOMP_KEY_DIAG_NORM,
 * default 1) they are scaled back onto the circle (23170, 23170). One key alone is unchanged. */
static int keyboard_diagonal_normalise(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = pc_switch("RECOMP_KEY_DIAG_NORM", "Input", "DiagonalNormalise", 1) ? 1 : 0;
        fprintf(stderr, "[INPUT] keyboard diagonal normalise=%d (W+D stick magnitude %s)\n", enabled,
                enabled ? "1.00" : "1.41");
    }
    return enabled;
}
static void left_stick_from_keys(XBOX_INPUT_STATE *state, int left, int right, int down, int up)
{
    int x = axis_from_keys(left, right), y = axis_from_keys(down, up);
    if (keyboard_diagonal_normalise()) pc_stick_clamp_circle(&x, &y);
    state->Gamepad.sThumbLX = (SHORT)x;
    state->Gamepad.sThumbLY = (SHORT)y;
}

/* The arrow keys (D-pad) and I/J/K/L (right stick) are hard-wired. In play, one
 * the user has also bound to an action must do only that action, not two. */
static BOOL vk_user_bound(int vk)
{
    for (int i = 0; i < PC_BINDING_COUNT; i++)
        if (pc_binding_get((enum PcBinding)i, -1) == vk) return TRUE;
    return FALSE;
}
static BOOL fixed_key_down(int vk)
{
    return key_down(vk) && !(pc_input_context() == PC_INPUT_GAMEPLAY && vk_user_bound(vk));
}
static SHORT axis_from_fixed_keys(int negative, int positive)
{
    int v = 0;
    if (fixed_key_down(negative)) v -= 32767;
    if (fixed_key_down(positive)) v += 32767;
    return (SHORT)v;
}

/* Mouse movement drives the guest's right stick. Consume each raw-input
 * delta once, and clear the stick on the next poll when the mouse stops. */
static SHORT mouse_axis(int delta, double elapsed_seconds)
{
    static int sensitivity;
    long long value;
    if (!sensitivity) {
        const char *v = getenv("RECOMP_MOUSE_SENSITIVITY");
        sensitivity = v ? atoi(v) : 100;
        if (sensitivity < 1) sensitivity = 1;
        if (sensitivity > 500) sensitivity = 500;
    }
    if (!delta) return 0;
    if(elapsed_seconds<.002)elapsed_seconds=.002;
    if(elapsed_seconds>.1)elapsed_seconds=.1;
    value = (long long)((double)delta * 1200 * pc_setting_get(PC_MOUSE_SENSITIVITY, sensitivity) / (elapsed_seconds*6000));
    value += delta > 0 ? 6000 : -6000;
    if (value > 32767) value = 32767;
    if (value < -32767) value = -32767;
    return (SHORT)value;
}

static BOOL mouse_at_guest_poll(void)
{
    static int enabled=-1;
    if(enabled<0) {const char *v=getenv("RECOMP_MOUSE_GUEST_POLL");enabled=v&&atoi(v)!=0;}
    return enabled;
}

void xbox_InputReadMouse(XBOX_INPUT_STATE *state,double elapsed_seconds)
{
    static DWORD mouse_packet;
    if(!state || !mouse_at_guest_poll() || !keyboard_enabled() || pc_frontend_captured() || pc_input_text_captured() || !pc_setting_get(PC_MOUSE_AIM,1))return;
    int dx=0,dy=0;xbox_FramebufferMouseDelta(&dx,&dy);
    if(dx)state->Gamepad.sThumbRX=mouse_axis(dx,elapsed_seconds);
    if(dy)state->Gamepad.sThumbRY=mouse_axis(pc_setting_get(PC_INVERT_Y,0)?dy:-dy,elapsed_seconds);
    if(dx || dy)mouse_packet++;
    state->dwPacketNumber+=mouse_packet;
}

BOOL xbox_InputReadMouseAxes(float *x,float *y,double elapsed_seconds)
{
    *x=*y=0;
    if(!keyboard_enabled() || pc_frontend_captured() || pc_input_text_captured() || !pc_setting_get(PC_MOUSE_AIM,1))return FALSE;
    int dx=0,dy=0;xbox_FramebufferMouseDelta(&dx,&dy);
    if(!dx && !dy)return FALSE;
    if(!isfinite(elapsed_seconds) || elapsed_seconds<.001)elapsed_seconds=.001;
    if(elapsed_seconds>.1)elapsed_seconds=.1;
    double scale=.001*pc_setting_get(PC_MOUSE_SENSITIVITY,100)/(100*elapsed_seconds);
    *x=(float)(dx*scale);
    *y=(float)((pc_setting_get(PC_INVERT_Y,0)?dy:-dy)*scale);
    return TRUE;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    WORD b = 0;

    memset(pState, 0, sizeof(*pState));

    /* Host text must not also become an Xbox button, held stick or mouse aim.
     * Keep explicit menu clicks/cancel delivery, including with bindings off. */
    if (pc_input_text_captured()) {
        if (pc_input_context()==PC_INPUT_MENU)
            for (unsigned button=0;button<8;button++)
                if (pc_input_menu_button_pending(button,0))
                    pState->Gamepad.bAnalogButtons[button]=255;
        pState->dwPacketNumber=++packet;
        return;
    }

    if (fixed_key_down(VK_UP))     b |= XBOX_GAMEPAD_DPAD_UP;
    if (fixed_key_down(VK_DOWN))   b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (fixed_key_down(VK_LEFT))   b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (fixed_key_down(VK_RIGHT))  b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (key_down(pc_binding_get(PC_START,VK_RETURN))) b |= XBOX_GAMEPAD_START;
    if (key_down(pc_binding_get(PC_BACK,VK_BACK)) || key_down(VK_ESCAPE)) b |= XBOX_GAMEPAD_BACK;
    if (key_down(pc_binding_get(PC_LEFT_THUMB,VK_SHIFT))) b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (key_down(pc_binding_get(PC_RIGHT_THUMB,VK_CONTROL))) b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    {
        const char *layout = getenv("RECOMP_KEYBOARD_LAYOUT");
        if (layout && !_stricmp(layout, "pc")) {
            int dx = 0, dy = 0;
            /* Movies can start between guest menu updates. */
            enum PcInputContext context=pc_display_movie_active()?PC_INPUT_MOVIE:pc_input_context();
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = key_down(pc_binding_get(PC_A,VK_SPACE)) ? 255 : 0;
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = key_down(pc_binding_get(PC_B,'F')) ? 255 : 0;
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = key_down(pc_binding_get(PC_X,'R')) ? 255 : 0;
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = key_down(pc_binding_get(PC_Y,'Q')) ? 255 : 0;
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = key_down(pc_binding_get(PC_WHITE,'1')) ? 255 : 0;
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = key_down(pc_binding_get(PC_BLACK,'2')) ? 255 : 0;
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
                (key_down(pc_binding_get(PC_MOUSE_ALT_FIRE,VK_RBUTTON)) || key_down(pc_binding_get(PC_LEFT_TRIGGER,'C'))) ? 255 : 0;
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
                (key_down(pc_binding_get(PC_MOUSE_FIRE,VK_LBUTTON)) || key_down(pc_binding_get(PC_RIGHT_TRIGGER,'V'))) ? 255 : 0;
            left_stick_from_keys(pState, pc_binding_get(PC_MOVE_LEFT,'A'), pc_binding_get(PC_MOVE_RIGHT,'D'),
                                 pc_binding_get(PC_MOVE_BACK,'S'), pc_binding_get(PC_MOVE_FORWARD,'W'));
            pState->Gamepad.sThumbRX = axis_from_fixed_keys('J', 'L');
            pState->Gamepad.sThumbRY = axis_from_fixed_keys('K', 'I');
            /* PC navigation is contextual; Xbox Back is not the game's
             * cancel button. Keep the gameplay bindings independently editable. */
            pState->Gamepad.wButtons &= ~XBOX_GAMEPAD_BACK;
            if(key_down(pc_binding_get(PC_BACK,VK_BACK)) && context==PC_INPUT_GAMEPLAY)
                pState->Gamepad.wButtons |= XBOX_GAMEPAD_BACK;
            if(context==PC_INPUT_MOVIE) {
                /* Retail movie players accept different subsets of buttons.
                 * Use Start consistently, including with rebound gameplay
                 * controls. A click must not become fire or a menu selection. */
                if(key_down(VK_RETURN)||key_down(VK_ESCAPE)||key_down(VK_SPACE)||key_down(VK_LBUTTON))
                    pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
                if(key_down(VK_LBUTTON)&&pc_binding_get(PC_MOUSE_FIRE,VK_LBUTTON)==VK_LBUTTON)
                    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]=0;
            } else if(context==PC_INPUT_MENU) {
                for(unsigned b=0;b<8;b++)if(pc_input_menu_button_pending(b,0))pState->Gamepad.bAnalogButtons[b]=255;
                if(key_down(VK_RETURN)) {
                    if(pc_binding_get(PC_START,VK_RETURN)==VK_RETURN)
                        pState->Gamepad.wButtons &= ~XBOX_GAMEPAD_START;
                    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]=255;
                }
                if(key_down(VK_ESCAPE)||key_down(VK_BACK))
                    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]=255;
            } else {
                if(key_down(VK_ESCAPE))pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
                if(context==PC_INPUT_TITLE && key_down(VK_LBUTTON)) {
                    pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
                    if(pc_binding_get(PC_MOUSE_FIRE,VK_LBUTTON)==VK_LBUTTON)
                        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]=0;
                }
            }
            if(!mouse_at_guest_poll() && pc_setting_get(PC_MOUSE_AIM,1)) {
                xbox_FramebufferMouseDelta(&dx, &dy);
                if (dx) pState->Gamepad.sThumbRX = mouse_axis(dx,1.0/60);
                if (dy) pState->Gamepad.sThumbRY = mouse_axis(pc_setting_get(PC_INVERT_Y,0)?dy:-dy,1.0/60);
            }
            pState->dwPacketNumber = ++packet;
            return;
        }
    }

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = key_down('Z') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = key_down('X') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = key_down('A') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = key_down('S') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = key_down('Q') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = key_down('E') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = key_down('1') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = key_down('3') ? 255 : 0;

    /* W/A/S/D would collide with the face buttons above, so the left thumb
     * shares the arrow keys' row on the numeric pad instead. */
    left_stick_from_keys(pState, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD2, VK_NUMPAD8);
    pState->Gamepad.sThumbRX = axis_from_keys('J', 'L');
    pState->Gamepad.sThumbRY = axis_from_keys('K', 'I');

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* A guest poll acknowledges the buttons it actually received, rather than
 * letting the faster USB worker discard a tap between slow game frames. */
void xbox_InputAcknowledge(const XBOX_INPUT_STATE *state)
{
    const XBOX_GAMEPAD *g = &state->Gamepad;
    const char *layout = getenv("RECOMP_KEYBOARD_LAYOUT");
    const int pc = layout && !_stricmp(layout, "pc");
    static const int digital_keys[] = {
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, VK_SHIFT, VK_CONTROL
    };
    static const int legacy_analog[] = { 'Z', 'X', 'A', 'S', 'E', 'Q', '1', '3' };
    static const int pc_analog[] = { VK_SPACE, 'F', 'R', 'Q', '2', '1', 'C', 'V' };
    for (int i = 0; i < 8; i++) {
        static const enum PcBinding digital_bindings[8] = {
            PC_START,PC_START,PC_START,PC_START,PC_START,PC_BACK,PC_LEFT_THUMB,PC_RIGHT_THUMB
        };
        static const enum PcBinding analog_bindings[8] = {
            PC_A,PC_B,PC_X,PC_Y,PC_BLACK,PC_WHITE,PC_LEFT_TRIGGER,PC_RIGHT_TRIGGER
        };
        if (g->wButtons & (1u << i)) xbox_FramebufferAcknowledgeKey(
            pc && i>=4?pc_binding_get(digital_bindings[i],digital_keys[i]):digital_keys[i]);
        if (g->bAnalogButtons[i])
            xbox_FramebufferAcknowledgeKey(pc ? pc_binding_get(analog_bindings[i],pc_analog[i]) : legacy_analog[i]);
    }
    if (g->wButtons & XBOX_GAMEPAD_BACK) xbox_FramebufferAcknowledgeKey(VK_ESCAPE);
    if (pc) {
        enum PcInputContext context=pc_display_movie_active()?PC_INPUT_MOVIE:pc_input_context();
        if(context==PC_INPUT_MOVIE && (g->wButtons&XBOX_GAMEPAD_START)) {
            xbox_FramebufferAcknowledgeKey(VK_RETURN);
            xbox_FramebufferAcknowledgeKey(VK_ESCAPE);
            xbox_FramebufferAcknowledgeKey(VK_SPACE);
            xbox_FramebufferAcknowledgeKey(VK_LBUTTON);
        } else if(context==PC_INPUT_MENU) {
            for(unsigned b=0;b<8;b++)if(g->bAnalogButtons[b])pc_input_menu_button_pending(b,1);
            if(g->bAnalogButtons[XBOX_BUTTON_A]){xbox_FramebufferAcknowledgeKey(VK_RETURN);pc_input_menu_clicked(1);}
            if(g->bAnalogButtons[XBOX_BUTTON_B]) {
                xbox_FramebufferAcknowledgeKey(VK_ESCAPE);xbox_FramebufferAcknowledgeKey(VK_BACK);
            }
        } else if(g->wButtons&XBOX_GAMEPAD_START) {
            xbox_FramebufferAcknowledgeKey(VK_ESCAPE);
            if(context==PC_INPUT_TITLE)xbox_FramebufferAcknowledgeKey(VK_LBUTTON);
        }
        if (g->bAnalogButtons[XBOX_BUTTON_LTRIGGER]) xbox_FramebufferAcknowledgeKey(pc_binding_get(PC_MOUSE_ALT_FIRE,VK_RBUTTON));
        if (g->bAnalogButtons[XBOX_BUTTON_RTRIGGER]) xbox_FramebufferAcknowledgeKey(pc_binding_get(PC_MOUSE_FIRE,VK_LBUTTON));
    }
    if (g->sThumbLX < 0) xbox_FramebufferAcknowledgeKey(pc ? pc_binding_get(PC_MOVE_LEFT,'A') : VK_NUMPAD4);
    if (g->sThumbLX > 0) xbox_FramebufferAcknowledgeKey(pc ? pc_binding_get(PC_MOVE_RIGHT,'D') : VK_NUMPAD6);
    if (g->sThumbLY < 0) xbox_FramebufferAcknowledgeKey(pc ? pc_binding_get(PC_MOVE_BACK,'S') : VK_NUMPAD2);
    if (g->sThumbLY > 0) xbox_FramebufferAcknowledgeKey(pc ? pc_binding_get(PC_MOVE_FORWARD,'W') : VK_NUMPAD8);
    if (g->sThumbRX < 0) xbox_FramebufferAcknowledgeKey('J');
    if (g->sThumbRX > 0) xbox_FramebufferAcknowledgeKey('L');
    if (g->sThumbRY < 0) xbox_FramebufferAcknowledgeKey('K');
    if (g->sThumbRY > 0) xbox_FramebufferAcknowledgeKey('I');
}

#if defined(_WIN32)
void xbox_InputInit(void)
{
    ensure_feedback_policy();
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = xi_get(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
    if (keyboard_enabled()) g_controller_connected[0] = TRUE;
}

static DWORD xi_get_state_core(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (dwPort==0 && pc_frontend_captured()) {
        static DWORD packet;
        memset(pState,0,sizeof(*pState));
        pState->dwPacketNumber=++packet;
        input_trace_observe(dwPort,pState);
        return ERROR_SUCCESS;
    }

    if (dwPort == 0) {
        BOOL lost = FALSE;
        result = xi_poll_active(&xi_state, &lost);
        if (lost && pc_input_device() == PC_INPUT_GAMEPAD && pc_input_context() == PC_INPUT_GAMEPLAY)
            xi_arm_pause_tap();
    } else result = xi_get(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && (keyboard_enabled() || pc_input_text_captured())) {
            g_controller_connected[dwPort] = TRUE;
            keyboard_state(pState);
            input_trace_observe(dwPort,pState);
            return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;
    if(dwPort==0)pc_input_note_gamepad(xi_state.Gamepad.wButtons,
        xi_state.Gamepad.bLeftTrigger,xi_state.Gamepad.bRightTrigger,
        xi_state.Gamepad.sThumbLX,xi_state.Gamepad.sThumbLY,
        xi_state.Gamepad.sThumbRX,xi_state.Gamepad.sThumbRY);

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    /* Switching to another slot restarts that pad's packet counter; the epoch keeps the number moving. */
    pState->dwPacketNumber = xi_state.dwPacketNumber +
        (dwPort == 0 ? (DWORD)InterlockedCompareExchange(&xi_switch_epoch, 0, 0) * 0x01000000u : 0u);
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad.
     *
     * The first version put the keyboard behind XInput's failure, on the
     * assumption that with nothing plugged in the call would fail. It does
     * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
     * with every button at rest, so the fallback was unreachable and
     * pressing a key did nothing at all.
     *
     * Merging is also the better rule. A real pad keeps working -- its
     * buttons are already in pState and the keyboard only adds to them --
     * and there is no special case left to get wrong. */
    if (dwPort == 0 && (keyboard_enabled() || pc_input_text_captured())) {
        XBOX_INPUT_STATE kb;
        int i;
        keyboard_state(&kb);
        pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
        for (i = 0; i < 8; i++)
            if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
        if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
        if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
        if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
        if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
        /* The input layer records edges, so an unchanged packet number is
         * read as the same state and the press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }

    input_trace_observe(dwPort,pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD result = xi_get_state_core(dwPort, pState);
    return dwPort == 0 && pState ? xi_apply_pause_tap(result, pState) : result;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    /* Silent owned probes still retire the guest USB transfer normally. */
    const char *silent=getenv("RECOMP_RUMBLE");
    if(silent && *silent=='0') {
        if(input_trace_option(&rumble_trace_enabled,"RECOMP_RUMBLE_TRACE")) {
            XINPUT_VIBRATION stopped={0};
            AcquireSRWLockExclusive(&g_feedback.lock);
            feedback_trace("silent_environment",dwPort,xi_vib,stopped,ERROR_SUCCESS,g_feedback.focused,0);
            ReleaseSRWLockExclusive(&g_feedback.lock);
        }
        return ERROR_SUCCESS;
    }
    /* USB output is the demonstrated live path. Optional InputInit must not
     * be required for a pause/source transition to stop an already sent effect. */
    ensure_feedback_policy();
    {
        DWORD result = xinput_feedback_send(&g_feedback,dwPort,xi_vib,xi_send_mapped);
        if (result == ERROR_DEVICE_NOT_CONNECTED && dwPort == 0 && keyboard_enabled())
            return ERROR_SUCCESS;
        return result;
    }
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES caps_data;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = xi_caps(xi_host_slot(dwPort), dwFlags, &caps_data);
    if (result != ERROR_SUCCESS) {
        if (dwPort != 0 || !keyboard_enabled()) return result;
        memset(pCaps, 0, sizeof(*pCaps));
        pCaps->Type = XINPUT_DEVTYPE_GAMEPAD;
        pCaps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        pCaps->Gamepad.wButtons = 0x00FF;
        memset(pCaps->Gamepad.bAnalogButtons, 255, 8);
        pCaps->Gamepad.sThumbLX = pCaps->Gamepad.sThumbLY = 32767;
        pCaps->Gamepad.sThumbRX = pCaps->Gamepad.sThumbRY = 32767;
        return ERROR_SUCCESS;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = caps_data.Type;
    pCaps->SubType = caps_data.SubType;
    pCaps->Flags = caps_data.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static uint32_t g_previous[XBOX_MAX_CONTROLLERS], g_packet[XBOX_MAX_CONTROLLERS];
static int g_seeded[XBOX_MAX_CONTROLLERS], g_active, g_ready, g_disconnect_pending;
static int g_controller_owned;
static XBOX_GAMEPAD g_packet_state[XBOX_MAX_CONTROLLERS];
static unsigned char g_packet_valid[XBOX_MAX_CONTROLLERS];
static SRWLOCK g_packet_lock = SRWLOCK_INIT;
static SDL_atomic_t g_main_pump, g_focus, g_blocked;
static volatile LONGLONG g_pause_until;

void xbox_InputSetMainPump(int on) { SDL_AtomicSet(&g_main_pump, on != 0); }

static void update_controllers(void)
{
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) && !SDL_AtomicGet(&g_main_pump))
        SDL_GameControllerUpdate();
}

static int controller_open_locked(SDL_JoystickID id)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; ++i) {
        SDL_Joystick *j = g_pads[i] ? SDL_GameControllerGetJoystick(g_pads[i]) : NULL;
        if (j && SDL_JoystickInstanceID(j) == id) return 1;
    }
    return 0;
}

/* SDL's joystick lock also serializes handle removal with the event pump. */
static int refresh_controllers_locked(void)
{
    int lost_active = 0;
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; ++i) {
        if (g_pads[i] && !SDL_GameControllerGetAttached(g_pads[i])) {
            if (i == g_active) lost_active = 1;
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = NULL;
            g_seeded[i] = 0;
            fprintf(stderr, "[PC-INPUT] controller disconnected from slot %d\n", i);
        }
    }
    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        if (id < 0 || controller_open_locked(id)) continue;
        int slot;
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS && g_pads[slot]; ++slot) {}
        if (slot == XBOX_MAX_CONTROLLERS) break;
        SDL_GameController *controller = SDL_GameControllerOpen(i);
        if (controller) {
            g_pads[slot] = controller;
            g_seeded[slot] = 0;
            fprintf(stderr, "[PC-INPUT] controller connected in slot %d\n", slot);
        }
    }
    return lost_active;
}

static void sample_controller(SDL_GameController *c, XBOX_GAMEPAD *g)
{
    memset(g, 0, sizeof(*g));
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) g->wButtons |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) g->wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) g->wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) g->wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) g->wButtons |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) g->wButtons |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK)) g->wButtons |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) g->wButtons |= XBOX_GAMEPAD_RIGHT_THUMB;
    g->bAnalogButtons[XBOX_BUTTON_A] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_B] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_X] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_Y] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_BLACK] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_WHITE] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;
    int lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    g->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = (BYTE)(lt > 0 ? lt >> 7 : 0);
    g->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = (BYTE)(rt > 0 ? rt >> 7 : 0);
    g->sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    g->sThumbLY = (SHORT)(ly == INT16_MIN ? INT16_MAX : -ly);
    g->sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    int ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    g->sThumbRY = (SHORT)(ry == INT16_MIN ? INT16_MAX : -ry);
}

static uint32_t button_mask(const XBOX_GAMEPAD *g)
{
    uint32_t result = g->wButtons;
    for (int i = 0; i < 8; ++i)
        if (g->bAnalogButtons[i] > 30) result |= 1u << (8 + i);
    return result;
}

static int poll_locked(XBOX_GAMEPAD states[XBOX_MAX_CONTROLLERS], int port, int *lost)
{
    int old = g_active, fresh = -1;
    g_disconnect_pending |= refresh_controllers_locked();
    *lost = port == 0 ? g_disconnect_pending : 0;
    if (port == 0) g_disconnect_pending = 0;
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; ++i) {
        if (!g_pads[i]) continue;
        sample_controller(g_pads[i], &states[i]);
        uint32_t mask = button_mask(&states[i]);
        if (g_seeded[i] && i != g_active && (mask & ~g_previous[i]) && fresh < 0)
            fresh = i;
        g_previous[i] = mask;
        g_seeded[i] = 1;
    }
    if (!g_pads[g_active]) {
        for (int i = 0; i < XBOX_MAX_CONTROLLERS; ++i)
            if (g_pads[i]) { g_active = i; break; }
    } else if (fresh >= 0) g_active = fresh;
    if (old != g_active && g_pads[old]) {
        SDL_GameControllerRumble(g_pads[old], 0, 0, 0);
        fprintf(stderr, "[PC-INPUT] active controller slot %d -> %d\n", old, g_active);
    }
    return g_pads[g_active] != NULL;
}

static int feedback_allowed(DWORD port)
{
    return SDL_AtomicGet(&g_focus) &&
        pc_input_context() == PC_INPUT_GAMEPLAY && !pc_frontend_captured() &&
        !(SDL_AtomicGet(&g_blocked) & (1u << port)) &&
        (port != 0 || pc_input_device() == PC_INPUT_GAMEPAD);
}

static void feedback_policy_changed(void)
{
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    SDL_LockJoysticks();
    for (int port = 0; port < XBOX_MAX_CONTROLLERS; ++port) {
        int slot = port == 0 ? g_active : port;
        if (!feedback_allowed((DWORD)port) && g_pads[slot])
            SDL_GameControllerRumble(g_pads[slot], 0, 0, 0);
    }
    SDL_UnlockJoysticks();
}

void xbox_InputFeedbackPaused(unsigned blocked_ports)
{
    SDL_AtomicSet(&g_blocked, (int)(blocked_ports & 15u));
    feedback_policy_changed();
}

void xbox_InputFeedbackFocus(BOOL focused)
{
    SDL_AtomicSet(&g_focus, focused != FALSE);
    feedback_policy_changed();
}

void xbox_InputPauseTap(const char *why)
{
    if (pc_input_context() != PC_INPUT_GAMEPLAY || pc_display_movie_active()) return;
    InterlockedExchange64(&g_pause_until, (LONGLONG)(GetTickCount64() + 200));
    fprintf(stderr, "[PC-INPUT] %s: START tap to pause\n", why ? why : "pause");
}

void xbox_InputInit(void)
{
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            fprintf(stderr, "[PC-INPUT] SDL controller initialization failed: %s\n", SDL_GetError());
            return;
        }
        g_controller_owned = 1;
    }
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    SDL_AtomicSet(&g_focus, 1);
    SDL_LockJoysticks();
    refresh_controllers_locked();
    SDL_UnlockJoysticks();
    pc_input_register_output_policy_callback(feedback_policy_changed);
}

void xbox_InputShutdown(void)
{
    pc_input_register_output_policy_callback(NULL);
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    SDL_LockJoysticks();
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; ++i) {
        if (g_pads[i]) SDL_GameControllerClose(g_pads[i]);
        g_pads[i] = NULL;
        g_seeded[i] = 0;
    }
    g_active = 0;
    g_disconnect_pending = 0;
    __atomic_store_n(&g_ready, 0, __ATOMIC_RELEASE);
    SDL_UnlockJoysticks();
    if (g_controller_owned) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        g_controller_owned = 0;
    }
    InterlockedExchange64(&g_pause_until, 0);
    AcquireSRWLockExclusive(&g_packet_lock);
    memset(g_packet, 0, sizeof(g_packet));
    memset(g_packet_valid, 0, sizeof(g_packet_valid));
    ReleaseSRWLockExclusive(&g_packet_lock);
}

DWORD xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *state)
{
    if (port >= XBOX_MAX_CONTROLLERS || !state) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    XBOX_GAMEPAD samples[XBOX_MAX_CONTROLLERS] = {{0}};
    int lost = 0;
    memset(state, 0, sizeof(*state));

    if (port == 0 && pc_frontend_captured()) result = ERROR_SUCCESS;
    else {
        if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) {
            update_controllers();
            SDL_LockJoysticks();
            int any = __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) ?
                poll_locked(samples, (int)port, &lost) : 0;
            int slot = port == 0 ? g_active : (int)port;
            if ((port == 0 ? any : g_pads[slot] != NULL)) {
                state->Gamepad = samples[slot];
                result = ERROR_SUCCESS;
            }
            SDL_UnlockJoysticks();
            if (port == 0 && lost && pc_input_device() == PC_INPUT_GAMEPAD)
                xbox_InputPauseTap("active controller disconnected");
            if (port == 0 && result == ERROR_SUCCESS) {
                XBOX_GAMEPAD *g = &state->Gamepad;
                pc_input_note_gamepad(button_mask(g), g->bAnalogButtons[XBOX_BUTTON_LTRIGGER],
                    g->bAnalogButtons[XBOX_BUTTON_RTRIGGER], g->sThumbLX, g->sThumbLY,
                    g->sThumbRX, g->sThumbRY);
            }
        }
        if (port == 0 && (keyboard_enabled() || pc_input_text_captured())) {
            XBOX_INPUT_STATE kb;
            keyboard_state(&kb);
            state->Gamepad.wButtons |= kb.Gamepad.wButtons;
            for (int i = 0; i < 8; ++i)
                if (kb.Gamepad.bAnalogButtons[i] > state->Gamepad.bAnalogButtons[i])
                    state->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
            if (kb.Gamepad.sThumbLX) state->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
            if (kb.Gamepad.sThumbLY) state->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
            if (kb.Gamepad.sThumbRX) state->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
            if (kb.Gamepad.sThumbRY) state->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
            result = ERROR_SUCCESS;
        }
    }
    if (port == 0 && (ULONGLONG)InterlockedCompareExchange64(&g_pause_until, 0, 0) > GetTickCount64()) {
        state->Gamepad.wButtons |= XBOX_GAMEPAD_START;
        result = ERROR_SUCCESS;
    }
    AcquireSRWLockExclusive(&g_packet_lock);
    if (result != ERROR_SUCCESS) {
        g_packet_valid[port] = 0;
    } else if (!g_packet_valid[port] ||
               memcmp(&g_packet_state[port], &state->Gamepad, sizeof(state->Gamepad)) != 0) {
        g_packet_state[port] = state->Gamepad;
        g_packet_valid[port] = 1;
        ++g_packet[port];
    }
    state->dwPacketNumber = g_packet[port];
    ReleaseSRWLockExclusive(&g_packet_lock);
    return result;
}

DWORD xbox_InputSetState(DWORD port, const XBOX_VIBRATION *vibration)
{
    if (port >= XBOX_MAX_CONTROLLERS || !vibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    const char *silent = getenv("RECOMP_RUMBLE");
    if (silent && *silent == '0') return ERROR_SUCCESS;
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
        return ERROR_DEVICE_NOT_CONNECTED;
    update_controllers();
    SDL_LockJoysticks();
    g_disconnect_pending |= refresh_controllers_locked();
    int slot = port == 0 ? g_active : (int)port;
    SDL_GameController *c = g_pads[slot];
    int ok = c ? SDL_GameControllerRumble(c,
        feedback_allowed(port) ? vibration->wLeftMotorSpeed : 0,
        feedback_allowed(port) ? vibration->wRightMotorSpeed : 0, 1000) : -1;
    SDL_UnlockJoysticks();
    return !c ? ERROR_DEVICE_NOT_CONNECTED : ok == 0 ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}

BOOL xbox_InputIsConnected(DWORD port)
{
    if (port >= XBOX_MAX_CONTROLLERS) return FALSE;
    XBOX_INPUT_STATE state;
    return xbox_InputGetState(port, &state) == ERROR_SUCCESS;
}

DWORD xbox_InputGetCapabilities(DWORD port, DWORD flags, XBOX_INPUT_CAPABILITIES *caps)
{
    (void)flags;
    if (!caps || !xbox_InputIsConnected(port)) return ERROR_DEVICE_NOT_CONNECTED;
    memset(caps, 0, sizeof(*caps));
    caps->Type = 1;
    caps->SubType = 1;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
