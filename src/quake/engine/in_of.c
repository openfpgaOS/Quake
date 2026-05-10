/*
 * in_of.c -- Quake input driver over the openfpgaOS SDK.
 *
 * Mapping (mirrors PocketQuake for muscle memory):
 *   D-pad Up/Down      → K_UPARROW / K_DOWNARROW (move / menu)
 *   D-pad Left/Right   → ',' / '.' in game (strafe), K_LEFT/RIGHT in menu
 *   A (right)          → K_RIGHTARROW in game (turn right), K_ENTER in menu
 *   B (bottom)         → 'z' in game (look down),          K_ENTER in menu
 *   X (top)            → 'a' (look up)
 *   Y (left)           → K_LEFTARROW (turn left)
 *   L1 / R1            → K_SPACE (jump) / K_CTRL (fire)
 *   Select             → '/' (change weapon)
 *   Start              → K_ESCAPE (menu)
 *   Left stick         → cmd->forwardmove / sidemove
 */

#include "quakedef.h"

#include "of.h"
#include "of_input.h"

#include <string.h>

/* key_dest is declared in keys.h (keydest_t enum). */

/* Edge-detect previous snapshot. */
static uint32_t prev_buttons;
static int      face_a_key_down, face_b_key_down;
static int      dpad_l_key_down, dpad_r_key_down;

static qboolean in_nav(void)
{
    return key_dest == key_menu || key_dest == key_console;
}

void IN_Init(void)    { prev_buttons = 0; }
void IN_Shutdown(void){ }
void IN_Commands(void){ }

/* Map pad buttons to Quake keys in a context-sensitive way. */
static void send_edge_key(uint32_t changed, uint32_t buttons,
                          uint32_t mask, int quake_key)
{
    if (!(changed & mask)) return;
    Key_Event(quake_key, (buttons & mask) ? true : false);
}

static void send_context_key(uint32_t changed, uint32_t buttons,
                             uint32_t mask, int nav_key, int game_key,
                             int *down_slot)
{
    if (!(changed & mask)) return;
    qboolean down = (buttons & mask) ? true : false;
    int key = down ? (in_nav() ? nav_key : game_key)
                   : (*down_slot ? *down_slot : (in_nav() ? nav_key : game_key));
    *down_slot = down ? key : 0;
    Key_Event(key, down);
}

void IN_SendKeyEvents(void)
{
    of_input_poll();
    of_input_state_t st;
    of_input_state(0, &st);

    uint32_t buttons = st.buttons;
    uint32_t changed = buttons ^ prev_buttons;

    /* D-pad vertical: always arrow keys (menu + game). */
    send_edge_key(changed, buttons, OF_BTN_UP,    K_UPARROW);
    send_edge_key(changed, buttons, OF_BTN_DOWN,  K_DOWNARROW);

    /* Context-sensitive d-pad horizontal. */
    send_context_key(changed, buttons, OF_BTN_LEFT,
                     K_LEFTARROW, ',', &dpad_l_key_down);
    send_context_key(changed, buttons, OF_BTN_RIGHT,
                     K_RIGHTARROW, '.', &dpad_r_key_down);

    /* Face A / right — turn right in game, confirm in menu. */
    send_context_key(changed, buttons, OF_BTN_A,
                     K_ENTER, K_RIGHTARROW, &face_a_key_down);
    /* Face B / bottom — look down in game, confirm in menu. */
    send_context_key(changed, buttons, OF_BTN_B,
                     K_ENTER, 'z', &face_b_key_down);
    /* Face X / top — look up (no menu function). */
    send_edge_key(changed, buttons, OF_BTN_X, 'a');
    /* Face Y / left — turn left. */
    send_edge_key(changed, buttons, OF_BTN_Y, K_LEFTARROW);

    send_edge_key(changed, buttons, OF_BTN_L1,     K_SPACE);
    send_edge_key(changed, buttons, OF_BTN_R1,     K_CTRL);
    send_edge_key(changed, buttons, OF_BTN_SELECT, '/');
    send_edge_key(changed, buttons, OF_BTN_START,  K_ESCAPE);

    prev_buttons = buttons;
}

void IN_Move(usercmd_t *cmd)
{
    of_input_state_t st;
    of_input_state(0, &st);

    /* Analog stick: signed int16, deadzone 16 (fraction of INT16_MAX). */
    int16_t lx = st.joy_lx, ly = st.joy_ly;

    /* Rescale to the legacy ±128 band the speed cvars are tuned against. */
    int sx = lx / 256;
    int sy = ly / 256;

    if (sx > -16 && sx < 16) sx = 0;
    if (sy > -16 && sy < 16) sy = 0;

    cmd->forwardmove += -sy * cl_forwardspeed.value / 128.0f; /* +up = forward */
    cmd->sidemove    +=  sx * cl_sidespeed.value    / 128.0f;

    /* L2 / R2 shoulder triggers act as strafe in game. */
    if (key_dest == key_game) {
        if (st.buttons & OF_BTN_L2) cmd->sidemove -= cl_sidespeed.value;
        if (st.buttons & OF_BTN_R2) cmd->sidemove += cl_sidespeed.value;
    }
}
