#include "input/xinput_xbox.h"
#include "platform/pc_frontend.h"
#include "usb/usb_gamepad.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int keys[256];
int xbox_FramebufferKeyDown(int vk) { return vk >= 0 && vk < 256 ? keys[vk] : 0; }
void xbox_FramebufferMouseDelta(int *dx, int *dy) { *dx = *dy = 0; }
void xbox_FramebufferAcknowledgeKey(int vk) { (void)vk; }

static struct { int calls; SDL_JoystickID id; Uint16 low, high; } feedback;
int SDLCALL SDL_GameControllerRumble(SDL_GameController *controller,
                                   Uint16 low, Uint16 high, Uint32 duration)
{
    (void)duration;
    ++feedback.calls;
    feedback.id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    feedback.low = low; feedback.high = high;
    return 0;
}
static int attach(void)
{
    SDL_VirtualJoystickDesc d;
    memset(&d, 0, sizeof(d));
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    d.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    d.name = "MA2 synthetic gamepad";
    return SDL_JoystickAttachVirtualEx(&d);
}
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #expr, SDL_GetError()); return 1; } } while (0)

int main(void)
{
    CHECK(SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0);
    CHECK(SDL_NumJoysticks() == 0);
    pc_setting_set(PC_KEYBOARD, 0);
    pc_input_set_context(PC_INPUT_GAMEPLAY);
    xbox_InputInit();
    XBOX_INPUT_STATE state;
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);

    int index0 = attach();
    CHECK(index0 >= 0 && SDL_IsGameController(index0));
    SDL_Joystick *j0 = SDL_JoystickOpen(index0);
    CHECK(j0);
    CHECK(SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768) == 0);
    CHECK(SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768) == 0);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(state.Gamepad.sThumbLY == 0 && state.Gamepad.sThumbRY == 0);
    DWORD neutral_packet = state.dwPacketNumber;
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS && state.dwPacketNumber == neutral_packet);
    CHECK(SDL_JoystickSetVirtualButton(j0, SDL_CONTROLLER_BUTTON_B, 1) == 0);
    CHECK(SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_LEFTX, -32768) == 0);
    CHECK(SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_LEFTY, -32768) == 0);
    CHECK(SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767) == 0);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255);
    CHECK(state.dwPacketNumber != neutral_packet);
    CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
    CHECK(state.Gamepad.sThumbLX == -32768 && state.Gamepad.sThumbLY == 32767);
    CHECK(pc_input_device() == PC_INPUT_GAMEPAD);

    uint8_t report[20];
    CHECK(usb_gamepad_report(0, report, sizeof(report)) == 20);
    CHECK(report[1] == 20 && report[5] == 255 && report[11] == 255);
    CHECK(report[12] == 0 && report[13] == 0x80 && report[14] == 0xff && report[15] == 0x7f);
    CHECK(usb_gamepad_report(0, report, 19) == 0);

    int index1 = attach();
    CHECK(index1 >= 0);
    SDL_Joystick *j1 = SDL_JoystickOpen(index1);
    CHECK(j1);
    CHECK(SDL_JoystickSetVirtualAxis(j1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768) == 0);
    CHECK(SDL_JoystickSetVirtualAxis(j1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768) == 0);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(SDL_JoystickSetVirtualButton(j1, SDL_CONTROLLER_BUTTON_A, 1) == 0);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 &&
          state.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 0);

    const uint8_t vibration[6] = {0, 6, 0x34, 0x12, 0x78, 0x56};
    CHECK(usb_gamepad_output(0, vibration, sizeof(vibration)) == 1);
    CHECK(feedback.id == SDL_JoystickInstanceID(j1) &&
          feedback.low == 0x1234 && feedback.high == 0x5678);
    pc_frontend_capture(1);
    CHECK(feedback.low == 0 && feedback.high == 0);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS &&
          state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    CHECK(usb_gamepad_output(0, vibration, sizeof(vibration)) == 1);
    CHECK(feedback.low == 0 && feedback.high == 0);
    pc_frontend_capture(0);
    xbox_InputFeedbackPaused(1);
    CHECK(usb_gamepad_output(0, vibration, sizeof(vibration)) == 1 && feedback.low == 0);
    xbox_InputFeedbackPaused(0);
    CHECK(usb_gamepad_output(0, vibration, sizeof(vibration)) == 1 && feedback.low == 0x1234);
    int calls_before = feedback.calls;
    setenv("RECOMP_RUMBLE", "0", 1);
    CHECK(usb_gamepad_output(0, vibration, sizeof(vibration)) == 1 && feedback.calls == calls_before);
    unsetenv("RECOMP_RUMBLE");

    SDL_JoystickClose(j1);
    CHECK(SDL_JoystickDetachVirtual(index1) == 0);
    CHECK(xbox_InputGetState(1, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(state.Gamepad.wButtons & XBOX_GAMEPAD_START);
    SDL_Delay(220);

    pc_setting_set(PC_KEYBOARD, 1);
    keys['Z'] = 1;
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 &&
          state.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255);
    pc_input_text_capture(1);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS &&
          state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    pc_input_text_capture(0);
    xbox_InputShutdown();
    CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0);
    xbox_InputInit();
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS &&
          state.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255);
    xbox_InputShutdown();
    SDL_JoystickClose(j0);
    CHECK(SDL_JoystickDetachVirtual(index0) == 0);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    CHECK(!SDL_WasInit(SDL_INIT_GAMECONTROLLER));
    xbox_InputInit();
    CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0);
    xbox_InputShutdown();
    CHECK(!SDL_WasInit(SDL_INIT_GAMECONTROLLER));
    SDL_Quit();
    puts("SDL input: mapping, OHCI reports, hotplug, capture, rumble, and restart passed");
    return 0;
}
