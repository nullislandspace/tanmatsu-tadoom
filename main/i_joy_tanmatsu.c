/* i_joy_tanmatsu.c -- USB HID gamepad for Tanmatsu */

#include <string.h>

#include "esp_log.h"

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_main.h"
#include "i_joy.h"

#include "hidhost.h"

static const char *TAG = "i_joy";

/* joybfire/joybstrafe/joybuse/joybspeed are defined in g_game.c */
int joyleft, joyright, joyup, joydown;
int usejoystick = 0;

/*
 * PrBoom knows four joystick buttons, bound by default to fire, strafe, speed and use, and takes
 * the direction as the sign of an x and a y. A modern pad has rather more than four buttons, so
 * the rest are sent as key presses instead, which is also how anything that is a menu action has
 * to arrive: the menu reads key events and never looks at joyxmove.
 *
 * Buttons are named by what the report descriptor calls them rather than by where the report
 * happens to hold them, since a pad is free to list them in any order and some list them
 * backwards. Button 1 is the bottom face button on every pad this has been tried against.
 */
#define PAD_A       0  /* Button 1, bottom face button */
#define PAD_B       1  /* Button 2, right face button */
#define PAD_X       2  /* Button 3, left face button */
#define PAD_Y       3  /* Button 4, top face button */
#define PAD_L1      4  /* Button 5, left shoulder */
#define PAD_R1      5  /* Button 6, right shoulder */
#define PAD_L2      6  /* Button 7, left trigger */
#define PAD_R2      7  /* Button 8, right trigger */
#define PAD_SELECT  8  /* Button 9 */
#define PAD_START   9  /* Button 10 */

/* Buttons that become one of the four PrBoom knows, and which of them */
static const struct {
    int pad;
    int joy_button;  /* index into joybuttons[], matching the joyb_* defaults */
} joy_button_map[] = {
    {PAD_A,  0},  /* fire */
    {PAD_R2, 0},  /* fire, for anyone who expects the trigger to shoot */
    {PAD_L2, 1},  /* strafe */
    {PAD_R1, 2},  /* speed */
    {PAD_B,  3},  /* use, so opening a door is the button next to fire */
};

/* Buttons that become a key press, in game and in the menu alike */
static const struct {
    int pad;
    int key;
} key_button_map[] = {
    {PAD_X,      '0'},          /* weapon toggle, the only weapon control PrBoom binds by default */
    {PAD_Y,      KEYD_ENTER},
    {PAD_L1,     KEYD_TAB},     /* automap */
    {PAD_SELECT, KEYD_TAB},
    {PAD_START,  KEYD_ESCAPE},  /* menu */
};

/* What the menu wants instead of a direction, since it reads key events and nothing else */
static const int menu_direction_key[4] = {KEYD_UPARROW, KEYD_DOWNARROW, KEYD_LEFTARROW, KEYD_RIGHTARROW};

#define BUTTON_MAP_COUNT (sizeof(key_button_map) / sizeof(key_button_map[0]))

/* Only changes are worth an event, so what the last poll saw is kept. A pad unplugged mid-press
 * never sends the release, so this is also what says which keys to let go of. */
static uint32_t prev_buttons;
static bool     prev_menu_direction[4];
static bool     prev_menu_active;
static bool     prev_connected;

static void post_key(int key, bool down)
{
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = down ? ev_keydown : ev_keyup;
    ev.data1 = key;
    D_PostEvent(&ev);
}

/* Let go of a held direction and clear the joystick side of the ticcmd. Called when the menu opens
 * or closes, since the two halves of this file express the same stick differently and whichever
 * one is being left has to stop pressing what it was pressing.
 *
 * The buttons are deliberately left alone: the button that opened the menu is still down, and
 * releasing it here would have the next poll see a fresh press and close the menu again. */
static void release_directions(void)
{
    for (int i = 0; i < 4; i++) {
        if (prev_menu_direction[i]) {
            post_key(menu_direction_key[i], false);
            prev_menu_direction[i] = false;
        }
    }

    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ev_joystick;
    D_PostEvent(&ev);
}

/* Let go of everything, for a pad that was unplugged mid-press. Its release never arrives in a
 * report, and a key left down has the player walking into a wall until something else is pressed. */
static void release_all(void)
{
    for (size_t i = 0; i < BUTTON_MAP_COUNT; i++) {
        if (prev_buttons & (1u << key_button_map[i].pad)) {
            post_key(key_button_map[i].key, false);
        }
    }
    prev_buttons = 0;

    release_directions();
}

void I_InitJoystick(void)
{
    usejoystick  = 1;
    prev_buttons = 0;
    memset(prev_menu_direction, 0, sizeof(prev_menu_direction));
    prev_menu_active = false;
    prev_connected   = false;
}

void I_PollJoystick(void)
{
    hid_gamepad_state_t pad;

    if (!hidhost_gamepad_get_state(&pad)) {
        if (prev_connected) {
            ESP_LOGI(TAG, "Gamepad gone, letting go of what it held");
            release_all();
            prev_connected = false;
        }
        return;
    }

    if (!prev_connected) {
        ESP_LOGI(TAG, "Gamepad in hand, %u buttons", (unsigned)pad.button_count);
        prev_connected = true;
    }

    /* The menu and the game want the same stick expressed differently, so a press that spans the
     * two would otherwise be delivered twice or not at all. Drop everything on the way through. */
    if ((bool)menuactive != prev_menu_active) {
        release_directions();
        prev_menu_active = menuactive;
    }

    /* Buttons past the four PrBoom knows, on the edges only */
    for (size_t i = 0; i < BUTTON_MAP_COUNT; i++) {
        uint32_t bit = 1u << key_button_map[i].pad;
        bool     now = (pad.usage_buttons & bit) != 0;
        bool     was = (prev_buttons & bit) != 0;
        if (now != was) {
            post_key(key_button_map[i].key, now);
        }
    }

    if (menuactive) {
        /* No joystick event while the menu is up: it reads keys, and a held direction would
         * otherwise also be turning the player around behind it. */
        const bool now[4] = {pad.up, pad.down, pad.left, pad.right};
        for (int i = 0; i < 4; i++) {
            if (now[i] != prev_menu_direction[i]) {
                post_key(menu_direction_key[i], now[i]);
                prev_menu_direction[i] = now[i];
            }
        }
    } else {
        event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ev_joystick;

        for (size_t i = 0; i < sizeof(joy_button_map) / sizeof(joy_button_map[0]); i++) {
            if (pad.usage_buttons & (1u << joy_button_map[i].pad)) {
                ev.data1 |= 1 << joy_button_map[i].joy_button;
            }
        }

        /* Sign is all PrBoom reads: it tests the value against zero and picks a fixed step. */
        ev.data2 = (pad.right ? 1 : 0) - (pad.left ? 1 : 0);
        ev.data3 = (pad.down ? 1 : 0) - (pad.up ? 1 : 0);

        D_PostEvent(&ev);
    }

    prev_buttons = pad.usage_buttons;
}
