#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Register before starting workers. Rendering stays on the GPU thread; window
 * callbacks must queue events instead of touching the immediate context. */
typedef void (*PcRenderCallback)(void *, void *, void *, unsigned, unsigned);
typedef int (*PcMessageCallback)(void *, unsigned, uintptr_t, intptr_t);
typedef void (*PcDeviceLostCallback)(void);
void pc_frontend_register(PcRenderCallback render, PcMessageCallback message);
void pc_frontend_register_device_lost(PcDeviceLostCallback callback);
/* GPU thread only, before releasing its device and immediate context. */
void pc_frontend_device_lost(void);
void pc_frontend_render(void *device, void *context, void *window, unsigned width, unsigned height);
int pc_frontend_message(void *window, unsigned message, uintptr_t wparam, intptr_t lparam);
int pc_frontend_captured(void);
void pc_frontend_capture(int captured);
enum PcSetting { PC_VSYNC, PC_FRAME_LIMIT, PC_ASPECT, PC_FILTER, PC_VOLUME,
    PC_MOUSE_AIM, PC_MOUSE_SENSITIVITY, PC_INVERT_Y, PC_KEYBOARD, PC_RENDER_SCALE, PC_SHOW_FPS, PC_DRAW_DISTANCE, PC_TEXTURE_QUALITY, PC_FMV_QUALITY, PC_INVERT_FLIGHT, PC_SHADER_PRELOAD, PC_SETTING_COUNT };
int pc_setting_get(enum PcSetting setting, int fallback);
void pc_setting_set(enum PcSetting setting, int value);
void pc_display_size(unsigned width, unsigned height);
void pc_render_size(unsigned width, unsigned height);
void pc_render_size_get(unsigned *width, unsigned *height);
/* Physical pixels for a scene target. Guest UI coordinates remain unchanged. */
void pc_display_render_extent(unsigned width, unsigned height, unsigned *pixel_width, unsigned *pixel_height);
float pc_display_aspect(float original);
void pc_display_movie(int change);
int pc_display_movie_active(void);
/* A title may mark a stretch of play as a non-interactive in-engine
 * cinematic: the cinematic view and frame cap below apply until it is
 * cleared, without changing the saved display settings. */
void pc_display_cinematic(int active);
int pc_display_cinematic_active(void);
/* How a cinematic is shown: PILLARBOX = original 4:3; FILL = rendered at output resolution, each shot
 * keeps its authored width and fills the screen (top and bottom cropped as needed, letterboxed shots
 * wider than the screen keep bars); FILL_16_9 = the same but a shot is cropped no further than 16:9
 * (slim side bars beyond); WIDE = like gameplay (wider view, shows past the authored set edges).
 * fps = frame cap while a cinematic is active, 0 = the normal frame limit. */
enum PcCinematicView { PC_CINEMATIC_PILLARBOX, PC_CINEMATIC_FILL_16_9, PC_CINEMATIC_FILL, PC_CINEMATIC_WIDE };
void pc_display_cinematic_config(int view, int fps);
/* Widest aspect a shot may be shown at while a fill view is active (0 = no fill view). */
float pc_display_cinematic_fill_cap(void);
/* A user skip remains queued until the fullscreen player handles it.
 * RECOMP_SAFE_START is rearmed once for each outermost movie lifecycle.
 * Console first-view/profile gates do not apply. */
void pc_input_movie_skip(void);
void pc_input_movie_skip_rearm(void);
int pc_input_movie_skip_pending(int acknowledge);
unsigned pc_display_render_width(unsigned width, unsigned scale);
enum PcBinding { PC_START, PC_BACK, PC_A, PC_B, PC_X, PC_Y, PC_WHITE, PC_BLACK,
    PC_LEFT_TRIGGER, PC_RIGHT_TRIGGER, PC_MOVE_LEFT, PC_MOVE_RIGHT, PC_MOVE_BACK,
    PC_MOVE_FORWARD, PC_LEFT_THUMB, PC_RIGHT_THUMB, PC_MOUSE_FIRE, PC_MOUSE_ALT_FIRE, PC_BINDING_COUNT };
int pc_binding_get(enum PcBinding action, int fallback);
void pc_binding_set(enum PcBinding action, int key);
enum PcInputDevice { PC_INPUT_KEYBOARD_MOUSE, PC_INPUT_GAMEPAD };
enum PcInputContext { PC_INPUT_GAMEPLAY, PC_INPUT_MENU, PC_INPUT_TITLE, PC_INPUT_MOVIE };
/* Publication is atomic, including first-output registration. Notification
 * runs on the changing thread; serialize it with in-flight motor packets. */
typedef void (*PcInputOutputPolicyCallback)(void);
void pc_input_register_output_policy_callback(PcInputOutputPolicyCallback callback);
void pc_input_set_context(enum PcInputContext context);
enum PcInputContext pc_input_context(void);
void pc_input_note_keyboard(void);
void pc_input_note_gamepad(unsigned buttons, unsigned left_trigger, unsigned right_trigger,
    int left_x, int left_y, int right_x, int right_y);
enum PcInputDevice pc_input_device(void);
unsigned pc_input_revision(void);
void pc_input_menu_click(void);
int pc_input_menu_clicked(int acknowledge);
void pc_input_menu_button(unsigned button);
int pc_input_menu_button_pending(unsigned button,int acknowledge);
int pc_input_camera_captured(void);
void pc_input_camera_capture(int capture);
/* Name entry suppresses keyboard bindings while retaining the physical pad. */
void pc_input_text_capture(int capture);
int pc_input_text_captured(void);
#define PC_WINDOW_CONFIG_MESSAGE 0x8051u
#define PC_WINDOW_CAPTURE_MESSAGE 0x8052u
#define PC_WINDOW_MOUSE_MESSAGE 0x8053u
#define PC_WINDOW_CAMERA_MESSAGE 0x8054u
#define PC_WINDOW_RENDERER_ERROR_MESSAGE 0x8055u
/* Play resumed from a menu with keyboard/mouse: capture for mouse aim. */
#define PC_WINDOW_AIM_MESSAGE 0x8056u
#ifdef __cplusplus
}
#endif
