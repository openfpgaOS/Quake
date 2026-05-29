/*
 * in_of.c -- Quake input driver over the openfpgaOS SDK.
 *
 * Mapping:
 *   D-pad Up/Down      -> move forward/back
 *   D-pad Left/Right   -> turn left/right
 *   L1 + D-pad L/R     -> strafe left/right
 *   L2 / R2            -> strafe left/right
 *   Left stick         -> move forward/back + turn
 *   Right stick        -> mouse look
 *   A                  -> fire
 *   R1 + A             -> look up
 *   B tap (<500 ms)    -> open/use
 *   B hold             -> run
 *   R1 + B             -> look down
 *   X                  -> jump
 *   R1 + X             -> next weapon
 *   Y                  -> crouch / move down
 *   R1 + Y             -> previous weapon
 *   Start              -> menu
 *   Select             -> automap/status overlay
 */

#include "quakedef.h"

#include "of.h"
#include "of_input.h"

#include <string.h>

extern int in_impulse;

#define PAD_B_HOLD_TIME 0.5f
#define PAD_DEADZONE    16

typedef enum {
    PAD_ACT_NONE = 0,
    PAD_ACT_FORWARD,
    PAD_ACT_BACK,
    PAD_ACT_LEFT,
    PAD_ACT_RIGHT,
    PAD_ACT_MOVELEFT,
    PAD_ACT_MOVERIGHT,
    PAD_ACT_ATTACK,
    PAD_ACT_LOOKUP,
    PAD_ACT_LOOKDOWN,
    PAD_ACT_JUMP,
    PAD_ACT_MOVEDOWN,
    PAD_ACT_SPEED,
    PAD_ACT_USE,
    PAD_ACT_SHOWSCORES
} pad_action_t;

typedef struct {
    pad_action_t action;
    int          key;
} pad_slot_t;

typedef enum {
    PAD_B_NONE = 0,
    PAD_B_PENDING,
    PAD_B_SPEED,
    PAD_B_LOOKDOWN
} pad_b_mode_t;

static uint32_t prev_buttons;
static pad_slot_t dpad_up_slot    = {PAD_ACT_NONE, K_AUX1};
static pad_slot_t dpad_down_slot  = {PAD_ACT_NONE, K_AUX2};
static pad_slot_t dpad_left_slot  = {PAD_ACT_NONE, K_AUX3};
static pad_slot_t dpad_right_slot = {PAD_ACT_NONE, K_AUX4};
static pad_slot_t a_slot          = {PAD_ACT_NONE, K_AUX5};
static pad_slot_t b_slot          = {PAD_ACT_NONE, K_AUX6};
static pad_slot_t x_slot          = {PAD_ACT_NONE, K_AUX7};
static pad_slot_t y_slot          = {PAD_ACT_NONE, K_AUX8};
static pad_slot_t select_slot     = {PAD_ACT_NONE, K_AUX9};
static pad_b_mode_t b_mode;
static qboolean b_physical_down;
static float b_down_time;
static qboolean x_chord_fired;
static qboolean y_chord_fired;

static qboolean in_nav(void)
{
    return key_dest == key_menu || key_dest == key_console;
}

static const char *pad_action_cmd(pad_action_t action)
{
    switch (action) {
    case PAD_ACT_FORWARD:    return "forward";
    case PAD_ACT_BACK:       return "back";
    case PAD_ACT_LEFT:       return "left";
    case PAD_ACT_RIGHT:      return "right";
    case PAD_ACT_MOVELEFT:   return "moveleft";
    case PAD_ACT_MOVERIGHT:  return "moveright";
    case PAD_ACT_ATTACK:     return "attack";
    case PAD_ACT_LOOKUP:     return "lookup";
    case PAD_ACT_LOOKDOWN:   return "lookdown";
    case PAD_ACT_JUMP:       return "jump";
    case PAD_ACT_MOVEDOWN:   return "movedown";
    case PAD_ACT_SPEED:      return "speed";
    case PAD_ACT_USE:        return "use";
    case PAD_ACT_SHOWSCORES: return "showscores";
    default:                 return NULL;
    }
}

static void queue_button_cmd(pad_action_t action, int key, qboolean down)
{
    const char *cmd = pad_action_cmd(action);
    char text[64];

    if (!cmd)
        return;
    sprintf(text, "%c%s %i\n", down ? '+' : '-', cmd, key);
    Cbuf_AddText(text);
}

static void set_slot_action(pad_slot_t *slot, pad_action_t action)
{
    if (slot->action == action)
        return;
    if (slot->action != PAD_ACT_NONE)
        queue_button_cmd(slot->action, slot->key, false);
    slot->action = action;
    if (slot->action != PAD_ACT_NONE)
        queue_button_cmd(slot->action, slot->key, true);
}

static void tap_action(pad_action_t action, int key)
{
    queue_button_cmd(action, key, true);
    queue_button_cmd(action, key, false);
}

static void clear_game_actions(void)
{
    set_slot_action(&dpad_up_slot, PAD_ACT_NONE);
    set_slot_action(&dpad_down_slot, PAD_ACT_NONE);
    set_slot_action(&dpad_left_slot, PAD_ACT_NONE);
    set_slot_action(&dpad_right_slot, PAD_ACT_NONE);
    set_slot_action(&a_slot, PAD_ACT_NONE);
    set_slot_action(&b_slot, PAD_ACT_NONE);
    set_slot_action(&x_slot, PAD_ACT_NONE);
    set_slot_action(&y_slot, PAD_ACT_NONE);
    set_slot_action(&select_slot, PAD_ACT_NONE);
    b_mode = PAD_B_NONE;
    b_physical_down = false;
    x_chord_fired = false;
    y_chord_fired = false;
}

void IN_ClearStates(void)
{
    clear_game_actions();
    prev_buttons = 0;
}

void IN_Init(void)    { IN_ClearStates(); }
void IN_Shutdown(void){ IN_ClearStates(); }
void IN_Commands(void){ }

/* Map pad buttons to Quake keys in a context-sensitive way. */
static void send_edge_key(uint32_t changed, uint32_t buttons,
                          uint32_t mask, int quake_key)
{
    if (!(changed & mask)) return;
    Key_Event(quake_key, (buttons & mask) ? true : false);
}

static int axis_scaled(int16_t value)
{
    int scaled = value / 256;

    if (scaled > -PAD_DEADZONE && scaled < PAD_DEADZONE)
        return 0;
    return scaled;
}

static void pad_prev_weapon(void)
{
    static const struct {
        int bit;
        int impulse;
    } weapons[] = {
        {IT_AXE,              1},
        {IT_SHOTGUN,          2},
        {IT_SUPER_SHOTGUN,    3},
        {IT_NAILGUN,          4},
        {IT_SUPER_NAILGUN,    5},
        {IT_GRENADE_LAUNCHER, 6},
        {IT_ROCKET_LAUNCHER,  7},
        {IT_LIGHTNING,        8},
    };
    int active = cl.stats[STAT_ACTIVEWEAPON];
    int active_idx = 0;
    int count = (int)(sizeof(weapons) / sizeof(weapons[0]));

    for (int i = 0; i < count; i++) {
        if (active == weapons[i].bit || (active & weapons[i].bit)) {
            active_idx = i;
            break;
        }
    }

    for (int step = 1; step <= count; step++) {
        int idx = (active_idx + count - step) % count;
        if (cl.items & weapons[idx].bit) {
            in_impulse = weapons[idx].impulse;
            return;
        }
    }
}

static void update_b_button(qboolean down, qboolean r1)
{
    if (!down) {
        if (b_mode == PAD_B_PENDING)
            tap_action(PAD_ACT_USE, b_slot.key);
        set_slot_action(&b_slot, PAD_ACT_NONE);
        b_mode = PAD_B_NONE;
        b_physical_down = false;
        return;
    }

    if (!b_physical_down) {
        b_physical_down = true;
        b_down_time = realtime;
        b_mode = PAD_B_NONE;
    }

    if (r1) {
        set_slot_action(&b_slot, PAD_ACT_LOOKDOWN);
        b_mode = PAD_B_LOOKDOWN;
        return;
    }

    if (b_mode == PAD_B_LOOKDOWN) {
        set_slot_action(&b_slot, PAD_ACT_NONE);
        b_down_time = realtime;
        b_mode = PAD_B_PENDING;
    } else if (b_mode == PAD_B_NONE) {
        b_mode = PAD_B_PENDING;
    }

    if (b_mode == PAD_B_PENDING &&
        realtime - b_down_time >= PAD_B_HOLD_TIME) {
        set_slot_action(&b_slot, PAD_ACT_SPEED);
        b_mode = PAD_B_SPEED;
    }
}

static void send_nav_events(uint32_t changed, uint32_t buttons)
{
    send_edge_key(changed, buttons, OF_BTN_UP,     K_UPARROW);
    send_edge_key(changed, buttons, OF_BTN_DOWN,   K_DOWNARROW);
    send_edge_key(changed, buttons, OF_BTN_LEFT,   K_LEFTARROW);
    send_edge_key(changed, buttons, OF_BTN_RIGHT,  K_RIGHTARROW);
    send_edge_key(changed, buttons, OF_BTN_A,      K_ENTER);
    send_edge_key(changed, buttons, OF_BTN_B,      K_ENTER);
    send_edge_key(changed, buttons, OF_BTN_START,  K_ESCAPE);
}

void IN_SendKeyEvents(void)
{
    of_input_poll();
    of_input_state_t st;
    of_input_state(0, &st);

    uint32_t buttons = st.buttons;
    uint32_t changed = buttons ^ prev_buttons;

    if (in_nav()) {
        clear_game_actions();
        send_nav_events(changed, buttons);
        prev_buttons = buttons;
        return;
    }

    qboolean l1 = (buttons & OF_BTN_L1) != 0;
    qboolean r1 = (buttons & OF_BTN_R1) != 0;
    qboolean x_down = (buttons & OF_BTN_X) != 0;
    qboolean y_down = (buttons & OF_BTN_Y) != 0;

    set_slot_action(&dpad_up_slot,
        (buttons & OF_BTN_UP) ? PAD_ACT_FORWARD : PAD_ACT_NONE);
    set_slot_action(&dpad_down_slot,
        (buttons & OF_BTN_DOWN) ? PAD_ACT_BACK : PAD_ACT_NONE);
    set_slot_action(&dpad_left_slot,
        (buttons & OF_BTN_LEFT)
            ? (l1 ? PAD_ACT_MOVELEFT : PAD_ACT_LEFT)
            : PAD_ACT_NONE);
    set_slot_action(&dpad_right_slot,
        (buttons & OF_BTN_RIGHT)
            ? (l1 ? PAD_ACT_MOVERIGHT : PAD_ACT_RIGHT)
            : PAD_ACT_NONE);

    set_slot_action(&a_slot,
        (buttons & OF_BTN_A)
            ? (r1 ? PAD_ACT_LOOKUP : PAD_ACT_ATTACK)
            : PAD_ACT_NONE);
    update_b_button((buttons & OF_BTN_B) != 0, r1);

    if (x_down && r1) {
        set_slot_action(&x_slot, PAD_ACT_NONE);
        if (!x_chord_fired) {
            in_impulse = 10;
            x_chord_fired = true;
        }
    } else {
        x_chord_fired = false;
        set_slot_action(&x_slot, x_down ? PAD_ACT_JUMP : PAD_ACT_NONE);
    }

    if (y_down && r1) {
        set_slot_action(&y_slot, PAD_ACT_NONE);
        if (!y_chord_fired) {
            pad_prev_weapon();
            y_chord_fired = true;
        }
    } else {
        y_chord_fired = false;
        set_slot_action(&y_slot, y_down ? PAD_ACT_MOVEDOWN : PAD_ACT_NONE);
    }

    set_slot_action(&select_slot,
        (buttons & OF_BTN_SELECT) ? PAD_ACT_SHOWSCORES : PAD_ACT_NONE);
    send_edge_key(changed, buttons, OF_BTN_START, K_ESCAPE);

    prev_buttons = buttons;
}

void IN_Move(usercmd_t *cmd)
{
    of_input_state_t st;
    of_input_state(0, &st);

    if (key_dest == key_game) {
        int lx = axis_scaled(st.joy_lx);
        int ly = axis_scaled(st.joy_ly);
        int rx = axis_scaled(st.joy_rx);
        int ry = axis_scaled(st.joy_ry);

        cmd->forwardmove += -ly * cl_forwardspeed.value / 128.0f;

        if (lx) {
            cl.viewangles[YAW] -= host_frametime * cl_yawspeed.value *
                                  (float)lx / 128.0f;
            cl.viewangles[YAW] = anglemod(cl.viewangles[YAW]);
        }
        if (rx) {
            cl.viewangles[YAW] -= host_frametime * cl_yawspeed.value *
                                  (float)rx / 128.0f;
            cl.viewangles[YAW] = anglemod(cl.viewangles[YAW]);
        }
        if (ry) {
            cl.viewangles[PITCH] += host_frametime * cl_pitchspeed.value *
                                    (float)ry / 128.0f;
            if (cl.viewangles[PITCH] > 80)
                cl.viewangles[PITCH] = 80;
            if (cl.viewangles[PITCH] < -70)
                cl.viewangles[PITCH] = -70;
            V_StopPitchDrift();
        }

        if (st.buttons & OF_BTN_L2) cmd->sidemove -= cl_sidespeed.value;
        if (st.buttons & OF_BTN_R2) cmd->sidemove += cl_sidespeed.value;
    }
}
