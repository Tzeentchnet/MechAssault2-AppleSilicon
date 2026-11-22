#include "bink_native.h"
#include "bots/ma2_bot_api.h"
#include "bots/ma2_bot_c7.h"
#include "bots/ma2_bots_core.h"
#include "ma2_tank_trace.h"
#include "pc_settings.h"
#include "scenario/ma2_scenario.h"
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"

#include <stddef.h>
#include <stdint.h>

extern ptrdiff_t g_xbox_mem_offset;

int c7_on(const char *piece, const char *environment, const char *key, int fallback)
{
    (void)piece;
    (void)environment;
    (void)key;
    (void)fallback;
    return 0;
}

void c7_add(const char *piece, const char *counter, double value)
{
    (void)piece;
    (void)counter;
    (void)value;
}

void c7_max(const char *piece, const char *counter, double value)
{
    (void)piece;
    (void)counter;
    (void)value;
}

double c7_t0(void)
{
    return 0.0;
}

void c7_ms(const char *piece, const char *counter, double started)
{
    (void)piece;
    (void)counter;
    (void)started;
}

int ma2_bink_decode(uint32_t movie, uint32_t video)
{
    (void)movie;
    (void)video;
    return 0;
}

int ma2_bink_copy(uint32_t movie, uint32_t destination, int pitch, int height,
                  int x, int y, uint32_t flags)
{
    (void)movie;
    (void)destination;
    (void)pitch;
    (void)height;
    (void)x;
    (void)y;
    (void)flags;
    return 0;
}

void ma2_bink_release(uint32_t movie)
{
    (void)movie;
}

void ma2_bink_opened(uint32_t movie, uint32_t name, uint32_t flags)
{
    (void)movie;
    (void)name;
    (void)flags;
}

int ma2_bot_actor_owned(uint32_t actor)
{
    (void)actor;
    return 0;
}

int ma2_bot_ai_select(uint32_t ai, uint32_t *selected)
{
    (void)ai;
    (void)selected;
    return 0;
}

int ma2_bot_cam_set(uint32_t slot, uint32_t table, uint32_t camera,
                    uint32_t return_address)
{
    (void)slot;
    (void)table;
    (void)camera;
    (void)return_address;
    return 0;
}

int ma2_bot_count_for(uint32_t session)
{
    (void)session;
    return 0;
}

int ma2_bot_entry_fix_on(void)
{
    return 0;
}

float ma2_bot_ground_y(float x, float z, int *known)
{
    (void)x;
    (void)z;
    if (known) *known = 0;
    return 0.0f;
}

void ma2_bot_hack_eject(uint32_t player, uint32_t enabled)
{
    (void)player;
    (void)enabled;
}

int ma2_bot_is_local_ai(uint32_t player)
{
    (void)player;
    return 0;
}

int ma2_bot_is_player(uint32_t player)
{
    (void)player;
    return 0;
}

void ma2_bot_lobby_record(int index, uint32_t session, uint32_t record)
{
    (void)index;
    (void)session;
    (void)record;
}

uint32_t ma2_bot_local_flags(uint32_t player)
{
    if (!g_xbox_mem_offset || player < UINT32_C(0x10000) ||
        player > UINT32_C(0x03FFFFFC) - UINT32_C(0x244))
        return 0;
    return *(const uint32_t *)((uintptr_t)g_xbox_mem_offset +
                              player + UINT32_C(0x244));
}

void ma2_bot_player_removed(uint32_t player)
{
    (void)player;
}

void ma2_bot_player_tick(uint32_t player)
{
    (void)player;
}

uint32_t ma2_bot_session_get(int field, uint32_t controller)
{
    (void)field;
    (void)controller;
    return 0;
}

int ma2_bot_session_set(int field, uint32_t controller, uint32_t value)
{
    (void)field;
    (void)controller;
    (void)value;
    return 0;
}

int ma2_bot_switch_on(int selector)
{
    (void)selector;
    return 0;
}

int ma2_bot_test_human_pad(float *values)
{
    (void)values;
    return 0;
}

void ma2_bot_trace(int identifier, uint32_t first, uint32_t second)
{
    (void)identifier;
    (void)first;
    (void)second;
}

uint32_t ma2_bot_vehicle_ai(uint32_t vehicle, uint32_t flags)
{
    (void)vehicle;
    return flags;
}

void ma2_bot_view_drawn(uint32_t view)
{
    (void)view;
}

void ma2_bot_vtol_lift(uint32_t actor)
{
    (void)actor;
}

int ma2_bot_world_pause_ok(void)
{
    return 0;
}

int ma2_bots_dragoon(void)
{
    return 0;
}

void ma2_bots_level_begin(uint32_t session)
{
    (void)session;
}

void ma2_bots_level_spawn(uint32_t session)
{
    (void)session;
}

int ma2_bots_lobby_active(uint32_t session)
{
    (void)session;
    return 0;
}

void ma2_c3_count(int counter)
{
    (void)counter;
}

int ma2_bot_air_blocked(float x, float z)
{
    (void)x;
    (void)z;
    return 0;
}

int ma2_bot_has_jets(uint32_t actor)
{
    (void)actor;
    return 0;
}

int ma2_bot_leap_near(int index, Ma2Vec3 position, float radius)
{
    (void)index;
    (void)position;
    (void)radius;
    return 0;
}

void ma2_bot_pad_c7(double now)
{
    (void)now;
}

void ma2_bot_pad_home(int nation, Ma2Vec3 position)
{
    (void)nation;
    (void)position;
}

int ma2_bot_pad_near(Ma2Vec3 from, Ma2Vec3 *result)
{
    (void)from;
    (void)result;
    return 0;
}

int ma2_bot_pad_pick(const Ma2World *world, const Ma2PlayerView *player,
                     Ma2Vec3 home, unsigned skip_mask, Ma2Vec3 *result,
                     int *index)
{
    (void)world;
    (void)player;
    (void)home;
    (void)skip_mask;
    (void)result;
    (void)index;
    return 0;
}

int ma2_bot_turret_near(float x, float z, float radius)
{
    (void)x;
    (void)z;
    (void)radius;
    return 0;
}

int ma2_bot_vtol_pads_on(void)
{
    return 0;
}

int ma2_nav_connected(const Ma2Nav *navigation, Ma2Vec3 from, Ma2Vec3 to)
{
    (void)navigation;
    (void)from;
    (void)to;
    return 0;
}

int ma2_nav_node_count(const Ma2Nav *navigation)
{
    (void)navigation;
    return 0;
}

Ma2Vec3 ma2_nav_node_pos(const Ma2Nav *navigation, int node)
{
    Ma2Vec3 result = {0.0f, 0.0f, 0.0f};
    (void)navigation;
    (void)node;
    return result;
}

int ma2_nav_path(Ma2Nav *navigation, Ma2Vec3 from, Ma2Vec3 to,
                 Ma2Vec3 *result, int capacity)
{
    (void)navigation;
    (void)from;
    (void)to;
    (void)result;
    (void)capacity;
    return 0;
}

int ma2_trav_changed_since(unsigned epoch, float x0, float z0,
                           float x1, float z1)
{
    (void)epoch;
    (void)x0;
    (void)z0;
    (void)x1;
    (void)z1;
    return 1;
}

float ma2_trav_clear(float x, float z, double now)
{
    (void)x;
    (void)z;
    (void)now;
    return 0.0f;
}

unsigned ma2_trav_epoch(void)
{
    return 0;
}

int ma2_trav_ready(void)
{
    return 0;
}

int ma2_trav_view(Ma2TravView *view)
{
    (void)view;
    return 0;
}

int ma2_trav_view_heavy(Ma2TravView *view)
{
    (void)view;
    return 0;
}

void ma2_cine_camera_fill(uint32_t camera)
{
    (void)camera;
}

void ma2_cine_trace_camera(uint32_t camera)
{
    (void)camera;
}

float ma2_cine_fov(uint32_t camera, float fov)
{
    (void)camera;
    return fov;
}

int ma2_cine_authored_aspect(uint32_t return_address)
{
    (void)return_address;
    return 0;
}

uint32_t ma2_gui_find(uint32_t form, uint32_t action)
{
    (void)form;
    (void)action;
    return 0;
}

void ma2_gui_text(uint32_t widget, uint32_t text)
{
    (void)widget;
    (void)text;
}

int ma2_gui_qte_draw(uint32_t widget)
{
    (void)widget;
    return 0;
}

void ma2_pc_form_loading(uint32_t object, uint32_t filename)
{
    (void)object;
    (void)filename;
}

int ma2_pc_ini_get_int(const char *section, const char *key, int fallback)
{
    (void)section;
    (void)key;
    return fallback;
}

void ma2_pc_ini_set_int(const char *section, const char *key, int value)
{
    (void)section;
    (void)key;
    (void)value;
}

void ma2_pc_input_context_update(void)
{
}

uint32_t ma2_pc_localized(uint32_t identifier, uint32_t original)
{
    (void)identifier;
    return original;
}

void ma2_pc_menu_pulse(uint32_t object, uint32_t vtable)
{
    (void)object;
    (void)vtable;
}

void ma2_pc_menu_selection(uint32_t object, uint32_t selected, int activated)
{
    (void)object;
    (void)selected;
    (void)activated;
}

void ma2_pc_modal_pulse(void)
{
}

uint32_t ma2_pc_prompt_text(uint32_t original)
{
    return original;
}

void ma2_pc_submenu_destroyed(uint32_t object)
{
    (void)object;
}

void ma2_pc_submenu_result(uint32_t object, int result)
{
    (void)object;
    (void)result;
}

int ma2_pc_submenu_update(uint32_t object, uint32_t vtable)
{
    (void)object;
    (void)vtable;
    return -1;
}

int ma2_pc_video_menu_update(uint32_t object)
{
    (void)object;
    return 2;
}

uint32_t ma2_pc_wide(const wchar_t *text)
{
    (void)text;
    return 0;
}

void ma2_scn_action_cue(uint32_t cue)
{
    (void)cue;
}

void ma2_scn_corporeal_build(void)
{
    sub_000D75C7();
}

int ma2_scn_custom_enter(void)
{
    return 0;
}

void ma2_scn_gamecond_execute(void)
{
    sub_0014E259();
}

void ma2_scn_hub_action(uint32_t action)
{
    (void)action;
}

void ma2_scn_level_load(void)
{
}

void ma2_scn_level_unload(void)
{
}

void ma2_scn_menu_frame(uint32_t object, uint32_t vtable)
{
    (void)object;
    (void)vtable;
}

void ma2_scn_mission_picker(uint32_t object)
{
    (void)object;
}

void ma2_scn_mount(uint32_t filesystem)
{
    (void)filesystem;
}

void ma2_scn_msg_execute(void)
{
    sub_001A0991();
}

void ma2_scn_player_update(void)
{
    sub_000C80DB();
}

void ma2_scn_resource_path(uint32_t buffer)
{
    (void)buffer;
}

void ma2_tank_trace(uint32_t site, uint32_t actor, uint32_t root,
                    uint32_t argument, uint32_t value)
{
    (void)site;
    (void)actor;
    (void)root;
    (void)argument;
    (void)value;
}
