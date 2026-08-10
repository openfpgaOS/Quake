/*
 * in_of.c -- Quake input driver over the openfpgaOS SDK.
 *
 * Mapping:
 *   D-pad Up/Down      -> move forward/back
 *   D-pad Left/Right   -> turn left/right
 *   L1 + D-pad L/R     -> strafe left/right
 *   L2                 -> run
 *   R2                 -> fire
 *   Left stick         -> move forward/back + strafe left/right
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
 *   Dock mouse         -> look + MOUSE1..3 (see read_mouse / IN_MouseMove)
 */

#include "quakedef.h"

#include "of.h"
#include "of_input.h"
#include "of_analogizer.h"

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

/* Analog axes are trusted only after one of them produces a value a
 * live, centred-at-rest stick can produce.  Digital pads on the SNAC
 * link port have no sticks: their undriven APF joy fields reach us
 * pinned at INT16_MIN every poll, which would otherwise decode as a
 * permanent full up-left deflection (ghost forward-move + left yaw).
 * A real stick rests near 0 and leaves {0, INT16_MIN} the moment it
 * is touched, so the latch engages immediately for analog controllers
 * and never for SNAC pads (see pad_axis_live / IN_Move). */
static int pad_analog_seen;

/* Dock-pad look sensitivity, 0..1 (archived in the config).  Applied to
 * the right-stick view rate only — full stick deflection still moves at
 * full speed, and a SNAC DualShock (PSX Analog) keeps the full look rate
 * since its pots already feel right at 1:1.
 *
 * Named joy_docklook (was joy_docksens): renamed so configs that
 * archived an old value during tuning stop pinning the shipped
 * default.  Deliberately NOT archived while the default is being
 * tuned — an archived copy in quake.cfg would pin old values and
 * mask every default change (which is exactly what happened during
 * the joy_docksens era). */
cvar_t joy_docklook = {"joy_docklook", "0.8", false};

/* A dock mouse aims by default, without holding +mlook — the same feel
 * as the right stick, which already pitches the view directly.  Set
 * "freelook 0" for the vanilla scheme, where mouse forward/back walks
 * and only +mlook (MOUSE3 in the stock binds) looks up and down.
 * Archived: this is a player preference, not a tuning default. */
cvar_t freelook = {"freelook", "1", true};

/* True when P1's analog axes come from a SNAC PSX-Analog pad rather
 * than the dock/Bluetooth controller. */
static int pad_snac_analog_p1(void)
{
    of_analogizer_state_t st;

    if (!of_analogizer_enabled())
        return 0;
    if (of_analogizer_state(&st) < 0 || !st.enabled)
        return 0;
    /* PSX Analog (0x12) / PSX Analog Fast (0x13) are the only SNAC
     * types with sticks. */
    if (st.snac_type != 0x12 && st.snac_type != 0x13)
        return 0;
    /* 0x40 (raw) / 1 (normalised) = "SNAC -> P2": P1 stays on the dock. */
    if (st.snac_assignment == 0x40 || st.snac_assignment == 1)
        return 0;
    return 1;
}
static pad_slot_t dpad_up_slot    = {PAD_ACT_NONE, K_AUX1};
static pad_slot_t dpad_down_slot  = {PAD_ACT_NONE, K_AUX2};
static pad_slot_t dpad_left_slot  = {PAD_ACT_NONE, K_AUX3};
static pad_slot_t dpad_right_slot = {PAD_ACT_NONE, K_AUX4};
static pad_slot_t a_slot          = {PAD_ACT_NONE, K_AUX5};
static pad_slot_t b_slot          = {PAD_ACT_NONE, K_AUX6};
static pad_slot_t x_slot          = {PAD_ACT_NONE, K_AUX7};
static pad_slot_t y_slot          = {PAD_ACT_NONE, K_AUX8};
static pad_slot_t select_slot     = {PAD_ACT_NONE, K_AUX9};
static pad_slot_t r2_slot         = {PAD_ACT_NONE, K_AUX10};
static pad_slot_t l2_slot         = {PAD_ACT_NONE, K_AUX11};
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
    set_slot_action(&r2_slot, PAD_ACT_NONE);
    set_slot_action(&l2_slot, PAD_ACT_NONE);
    b_mode = PAD_B_NONE;
    b_physical_down = false;
    x_chord_fired = false;
    y_chord_fired = false;
}

/* ---- Dock mouse ------------------------------------------------------ */

/* A USB mouse on the Analogue Dock aims alongside the pad, which keeps
 * doing everything else.  The three buttons arrive as MOUSE1..MOUSE3 so
 * the stock binds and the key menu own them; motion is banked here and
 * spent in IN_MouseMove.  With no mouse plugged in nothing is posted and
 * input behaves exactly as before. */

#define MOUSE_BUTTON_COUNT 3
#define MOUSE_ACCUM_LIMIT  32767

static uint16_t mouse_buttons;
static int mouse_accum_x, mouse_accum_y;

/* From caps v4 on the OS hands us decoded mouse counts; older firmware
 * passes the Pocket dock's packed int8 sample pairs {a<<8 | b} through
 * raw, where the true delta is the sum of the two halves. */
static int mouse_counts(int32_t v)
{
    static int raw_pairs = -1;

    if (raw_pairs < 0) {
        const struct of_capabilities *caps = of_get_caps();

        raw_pairs = caps != NULL
                 && caps->platform_id == OF_PLATFORM_POCKET
                 && (caps->version < 4
                  || !(caps->os_features & OF_OS_FEAT_MOUSE_COUNTS));
    }

    if (!raw_pairs)
        return v;

    return (int8_t)(v >> 8) + (int8_t)v;
}

static void mouse_accumulate(int *accum, int delta)
{
    int v = *accum + delta;

    if (v > MOUSE_ACCUM_LIMIT)
        v = MOUSE_ACCUM_LIMIT;
    else if (v < -MOUSE_ACCUM_LIMIT)
        v = -MOUSE_ACCUM_LIMIT;
    *accum = v;
}

static void release_mouse_buttons(void)
{
    for (int i = 0; i < MOUSE_BUTTON_COUNT; i++) {
        if (mouse_buttons & (1u << i))
            Key_Event(K_MOUSE1 + i, false);
    }
    mouse_buttons = 0;
}

/* of_input_mouse_state() consumes the motion and button edges the
 * firmware accumulated since the previous read, so this must stay its
 * only caller.  Called once per pass over the input, before the
 * menu/console early-out: button releases have to reach Key_Event even
 * when a menu opens mid-click, or the bound "+" action stays stuck. */
static void read_mouse(void)
{
    of_mouse_state_t ms;
    uint16_t changed;

    of_input_mouse_state(&ms);

    if (!ms.present) {
        release_mouse_buttons();     /* unplugged mid-click */
        mouse_accum_x = mouse_accum_y = 0;
        return;
    }

    changed = (uint16_t)(ms.buttons ^ mouse_buttons);
    mouse_buttons = ms.buttons;

    for (int i = 0; i < MOUSE_BUTTON_COUNT; i++) {
        if (changed & (1u << i))
            Key_Event(K_MOUSE1 + i, (ms.buttons & (1u << i)) != 0);
    }

    /* Only a live game spends banked motion — IN_Move runs from
     * CL_SendCmd, which bails unless the client is connected and past
     * the signon.  Dropping it in every other state keeps a menu visit
     * or a level load from ending in a view lurch. */
    if (key_dest != key_game || cls.state != ca_connected ||
        cls.signon != SIGNONS) {
        mouse_accum_x = mouse_accum_y = 0;
        return;
    }

    mouse_accumulate(&mouse_accum_x, mouse_counts(ms.dx));
    mouse_accumulate(&mouse_accum_y, mouse_counts(ms.dy));
}

/* Vanilla IN_MouseMove, with freelook standing in for a held +mlook. */
static void IN_MouseMove(usercmd_t *cmd)
{
    qboolean mlook;
    qboolean strafe;
    float mouse_x, mouse_y;

    if (!mouse_accum_x && !mouse_accum_y)
        return;

    mouse_x = (float)mouse_accum_x * sensitivity.value;
    mouse_y = (float)mouse_accum_y * sensitivity.value;
    mouse_accum_x = mouse_accum_y = 0;

    mlook = (in_mlook.state & 1) || freelook.value != 0.0f;
    strafe = (in_strafe.state & 1) != 0;

    if (strafe || (lookstrafe.value && mlook)) {
        cmd->sidemove += m_side.value * mouse_x;
    } else {
        cl.viewangles[YAW] -= m_yaw.value * mouse_x;
        cl.viewangles[YAW] = anglemod(cl.viewangles[YAW]);
    }

    if (mlook)
        V_StopPitchDrift();

    if (mlook && !strafe) {
        cl.viewangles[PITCH] += m_pitch.value * mouse_y;
        if (cl.viewangles[PITCH] > 80)
            cl.viewangles[PITCH] = 80;
        if (cl.viewangles[PITCH] < -70)
            cl.viewangles[PITCH] = -70;
    } else if (strafe && noclip_anglehack) {
        cmd->upmove -= m_forward.value * mouse_y;
    } else {
        cmd->forwardmove -= m_forward.value * mouse_y;
    }
}

void IN_ClearStates(void)
{
    clear_game_actions();
    release_mouse_buttons();
    mouse_accum_x = mouse_accum_y = 0;
    prev_buttons = 0;
    pad_analog_seen = 0;
}

void IN_Init(void)
{
    Cvar_RegisterVariable (&joy_docklook);
    Cvar_RegisterVariable (&freelook);
    IN_ClearStates();
}
void IN_Shutdown(void){ IN_ClearStates(); }
void IN_Commands(void){ }

/* Map pad buttons to Quake keys in a context-sensitive way. */
static void send_edge_key(uint32_t changed, uint32_t buttons,
                          uint32_t mask, int quake_key)
{
    if (!(changed & mask)) return;
    Key_Event(quake_key, (buttons & mask) ? true : false);
}

/* ── Real keyboard: Pocket dock, or MiSTer USB via hps_keyboard.v ─────
 *
 * of_keyboard_state_t reports USB HID usage IDs -- a level-triggered
 * 256-bit map plus pressed/released edge masks already computed by the
 * input HAL -- so this only translates and forwards.
 *
 * Quake takes lowercase ASCII for ordinary keys and its own K_* codes for
 * the rest, and has ONE code per modifier (K_CTRL/K_SHIFT/K_ALT), so left
 * and right collapse onto it.  Both the HID function-key block and Quake's
 * K_F1..K_F12 are contiguous, so that range maps arithmetically. */
static int hid_to_quakekey(unsigned usage)
{
    if (usage >= 0x04u && usage <= 0x1Du) return 'a' + (int)(usage - 0x04u);
    if (usage >= 0x1Eu && usage <= 0x26u) return '1' + (int)(usage - 0x1Eu);
    if (usage >= 0x3Au && usage <= 0x45u) return K_F1 + (int)(usage - 0x3Au);

    switch (usage) {
    case 0x27u: return '0';
    case 0x28u: return K_ENTER;
    case 0x29u: return K_ESCAPE;
    case 0x2Au: return K_BACKSPACE;
    case 0x2Bu: return K_TAB;
    case 0x2Cu: return K_SPACE;
    case 0x2Du: return '-';
    case 0x2Eu: return '=';
    case 0x2Fu: return '[';
    case 0x30u: return ']';
    case 0x31u: return '\\';
    case 0x33u: return ';';
    case 0x34u: return '\'';
    case 0x35u: return '`';
    case 0x36u: return ',';
    case 0x37u: return '.';
    case 0x38u: return '/';
    case 0x48u: return K_PAUSE;
    case 0x49u: return K_INS;
    case 0x4Au: return K_HOME;
    case 0x4Bu: return K_PGUP;
    case 0x4Cu: return K_DEL;
    case 0x4Du: return K_END;
    case 0x4Eu: return K_PGDN;
    case 0x4Fu: return K_RIGHTARROW;
    case 0x50u: return K_LEFTARROW;
    case 0x51u: return K_DOWNARROW;
    case 0x52u: return K_UPARROW;
    /* Keypad onto the main-row equivalents, so it works for menus and the
     * console without needing its own bindings. */
    case 0x58u: return K_ENTER;
    case 0x59u: return '1';
    case 0x5Au: return '2';
    case 0x5Bu: return '3';
    case 0x5Cu: return '4';
    case 0x5Du: return '5';
    case 0x5Eu: return '6';
    case 0x5Fu: return '7';
    case 0x60u: return '8';
    case 0x61u: return '9';
    case 0x62u: return '0';
    default:    return 0;
    }
}

static void poll_real_keyboard(void)
{
    static int prev_ctrl, prev_shift, prev_alt;
    of_keyboard_state_t kb;

    of_input_keyboard_state(&kb);
    if (!kb.present) {
        /* Unplugged mid-hold: release what we were reporting held. */
        if (prev_ctrl)  Key_Event(K_CTRL,  false);
        if (prev_shift) Key_Event(K_SHIFT, false);
        if (prev_alt)   Key_Event(K_ALT,   false);
        prev_ctrl = prev_shift = prev_alt = 0;
        return;
    }

    for (unsigned w = 0; w < OF_KEYBOARD_WORDS; w++) {
        uint32_t dn = kb.keys_pressed[w];
        uint32_t up = kb.keys_released[w];
        while (dn) {
            unsigned b = (unsigned)__builtin_ctz(dn);
            dn &= dn - 1u;
            int k = hid_to_quakekey(w * 32u + b);
            if (k) Key_Event(k, true);
        }
        while (up) {
            unsigned b = (unsigned)__builtin_ctz(up);
            up &= up - 1u;
            int k = hid_to_quakekey(w * 32u + b);
            if (k) Key_Event(k, false);
        }
    }

    /* Collapse L/R onto one code, then edge off the collapsed level -- so
     * holding LCtrl and adding RCtrl does not send a second keydown. */
    int ctrl  = (kb.modifiers & 0x11u) != 0;
    int shift = (kb.modifiers & 0x22u) != 0;
    int alt   = (kb.modifiers & 0x44u) != 0;
    if (ctrl  != prev_ctrl)  Key_Event(K_CTRL,  ctrl  ? true : false);
    if (shift != prev_shift) Key_Event(K_SHIFT, shift ? true : false);
    if (alt   != prev_alt)   Key_Event(K_ALT,   alt   ? true : false);
    prev_ctrl = ctrl; prev_shift = shift; prev_alt = alt;
}

static int pad_axis_live(int16_t v)
{
    return v != 0 && v != (int16_t)0x8000;
}

/* Softened response for the dock pad's movement stick: a 50/50 blend
 * of linear and squared, out = v*(128+|v|)/256 over the ±128 range.
 * Pure squared felt sluggish mid-range; this keeps walking speeds
 * reasonable (half tilt -> ~37%) while still calming the centre, and
 * full deflection reaches full speed.  SNAC DualShock sticks stay
 * linear (1:1). */
static int axis_curved(int v)
{
    int a = v < 0 ? -v : v;
    return v * (128 + a) / 256;
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

    read_mouse();
    /* Before the in_nav() early-out below, so the keyboard drives the menu
     * and console too, not just gameplay. */
    poll_real_keyboard();

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
    set_slot_action(&r2_slot,
        (buttons & OF_BTN_R2) ? PAD_ACT_ATTACK : PAD_ACT_NONE);
    set_slot_action(&l2_slot,
        (buttons & OF_BTN_L2) ? PAD_ACT_SPEED : PAD_ACT_NONE);
    send_edge_key(changed, buttons, OF_BTN_START, K_ESCAPE);

    prev_buttons = buttons;
}

void IN_Move(usercmd_t *cmd)
{
    of_input_state_t st;
    of_input_state(0, &st);

    /* SNAC / digital-pad guard: see pad_analog_seen above. */
    if (!pad_analog_seen) {
        if (pad_axis_live(st.joy_lx) || pad_axis_live(st.joy_ly) ||
            pad_axis_live(st.joy_rx) || pad_axis_live(st.joy_ry))
            pad_analog_seen = 1;
        else
            st.joy_lx = st.joy_ly = st.joy_rx = st.joy_ry = 0;
    }

    if (key_dest == key_game) {
        int dock = !pad_snac_analog_p1();
        int lx = axis_scaled(st.joy_lx);
        int ly = axis_scaled(st.joy_ly);
        int rx = axis_scaled(st.joy_rx);
        int ry = axis_scaled(st.joy_ry);

        if (dock) {
            lx = axis_curved(lx);   /* soften the dock movement stick */
            ly = axis_curved(ly);
        }

        cmd->forwardmove += -ly * cl_forwardspeed.value / 128.0f;
        cmd->sidemove   += lx * cl_sidespeed.value / 128.0f;

        float looksens = 1.0f;
        if (dock) {
            looksens = joy_docklook.value;
            if (looksens < 0.05f)      looksens = 0.05f;
            else if (looksens > 1.0f)  looksens = 1.0f;
        }

        if (rx) {
            cl.viewangles[YAW] -= host_frametime * cl_yawspeed.value *
                                  looksens * (float)rx / 128.0f;
            cl.viewangles[YAW] = anglemod(cl.viewangles[YAW]);
        }
        if (ry) {
            cl.viewangles[PITCH] += host_frametime * cl_pitchspeed.value *
                                    looksens * (float)ry / 128.0f;
            if (cl.viewangles[PITCH] > 80)
                cl.viewangles[PITCH] = 80;
            if (cl.viewangles[PITCH] < -70)
                cl.viewangles[PITCH] = -70;
            V_StopPitchDrift();
        }

        IN_MouseMove(cmd);
    }
}
