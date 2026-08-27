/* i_video_tanmatsu.c -- Display + Input for Tanmatsu (replaces SDL/i_video.c) */

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pax_gfx.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/device.h"

#include "doomstat.h"
#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "r_draw.h"
#include "d_main.h"
#include "d_event.h"
#include "i_joy.h"
#include "i_video.h"
#include "z_zone.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "lprintf.h"
#include "m_argv.h"

#include "hidhost.h"

static const char *TAG = "i_video";

/* Globals expected by PrBoom */
int gl_colorbuffer_bits = 16;
int gl_depthbuffer_bits = 16;
int use_doublebuffer = 0;
int use_fullscreen = 1;
int desired_fullscreen = 1;
int leds_always_off = 0;

/* External state from main.c */
extern size_t display_h_res;
extern size_t display_v_res;
extern lcd_rgb_data_endian_t display_data_endian;
extern pax_buf_t pax_framebuffer;
extern QueueHandle_t input_event_queue;

/* Internal state */
static uint8_t *doom_fb = NULL;       /* 320x200 8-bit framebuffer */
static uint32_t rgb_palette[256];     /* Current palette as 0x00RRGGBB */
static uint16_t scale_x[800];        /* Pre-computed source X for upscale */
static uint16_t scale_y[480];        /* Pre-computed source Y for upscale */
static size_t logical_w, logical_h;   /* Landscape dimensions after rotation */

/* Forward declarations */
static int tanmatsu_to_doom_key(uint32_t scancode);

/*
 * Scancode translation: PC Set-1 scancodes -> DOOM key constants.
 * BSP provides scancodes via INPUT_EVENT_TYPE_SCANCODE.
 * Release bit is 0x80 on the low byte.
 * Escaped scancodes have 0xE0xx prefix (already decoded by BSP as 0xE0xx values).
 */
static int tanmatsu_to_doom_key(uint32_t scancode)
{
    /* Strip release bit for lookup */
    uint32_t key = scancode & ~0x80;

    /* Check for escaped scancodes (0xE0xx prefix) */
    if (key >= 0xE000) {
        uint32_t esc = key & 0xFF;
        switch (esc) {
        case 0x48: return KEYD_UPARROW;
        case 0x50: return KEYD_DOWNARROW;
        case 0x4B: return KEYD_LEFTARROW;
        case 0x4D: return KEYD_RIGHTARROW;
        case 0x47: return KEYD_HOME;
        case 0x4F: return KEYD_END;
        case 0x49: return KEYD_PAGEUP;
        case 0x51: return KEYD_PAGEDOWN;
        case 0x52: return KEYD_INSERT;
        case 0x53: return KEYD_DEL;
        case 0x1D: return KEYD_RCTRL;
        case 0x38: return KEYD_RALT;
        case 0x1C: return KEYD_ENTER;   /* keypad enter */
        case 0x35: return '/';           /* keypad divide */
        default:   return 0;
        }
    }

    /* Standard scancodes */
    switch (key) {
    case 0x01: return KEYD_ESCAPE;
    case 0x1C: return KEYD_ENTER;
    case 0x0F: return KEYD_TAB;
    case 0x0E: return KEYD_BACKSPACE;
    case 0x39: return KEYD_SPACEBAR;

    /* Modifier keys */
    case 0x2A: return KEYD_RSHIFT;   /* Left Shift */
    case 0x36: return KEYD_RSHIFT;   /* Right Shift */
    case 0x1D: return KEYD_RCTRL;    /* Left Ctrl */
    case 0x38: return KEYD_RALT;     /* Left Alt */
    case 0x3A: return KEYD_CAPSLOCK;

    /* Function keys */
    case 0x3B: return KEYD_F1;
    case 0x3C: return KEYD_F2;
    case 0x3D: return KEYD_F3;
    case 0x3E: return KEYD_F4;
    case 0x3F: return KEYD_F5;
    case 0x40: return KEYD_F6;
    case 0x41: return KEYD_F7;
    case 0x42: return KEYD_F8;
    case 0x43: return KEYD_F9;
    case 0x44: return KEYD_F10;
    case 0x57: return KEYD_F11;
    case 0x58: return KEYD_F12;

    /* Number row: 1-9, 0 */
    case 0x02: return '1';
    case 0x03: return '2';
    case 0x04: return '3';
    case 0x05: return '4';
    case 0x06: return '5';
    case 0x07: return '6';
    case 0x08: return '7';
    case 0x09: return '8';
    case 0x0A: return '9';
    case 0x0B: return '0';

    /* Punctuation */
    case 0x0C: return KEYD_MINUS;
    case 0x0D: return KEYD_EQUALS;
    case 0x1A: return '[';
    case 0x1B: return ']';
    case 0x27: return ';';
    case 0x28: return '\'';
    case 0x29: return '`';
    case 0x2B: return '\\';
    case 0x33: return ',';
    case 0x34: return '.';
    case 0x35: return '/';

    /* Letter keys (QWERTY layout) -> lowercase */
    case 0x10: return 'q';
    case 0x11: return 'w';
    case 0x12: return 'e';
    case 0x13: return 'r';
    case 0x14: return 't';
    case 0x15: return 'y';
    case 0x16: return 'u';
    case 0x17: return 'i';
    case 0x18: return 'o';
    case 0x19: return 'p';
    case 0x1E: return 'a';
    case 0x1F: return 's';
    case 0x20: return 'd';
    case 0x21: return 'f';
    case 0x22: return 'g';
    case 0x23: return 'h';
    case 0x24: return 'j';
    case 0x25: return 'k';
    case 0x26: return 'l';
    case 0x2C: return 'z';
    case 0x2D: return 'x';
    case 0x2E: return 'c';
    case 0x2F: return 'v';
    case 0x30: return 'b';
    case 0x31: return 'n';
    case 0x32: return 'm';

    /* Pause */
    case 0x45: return KEYD_PAUSE;

    default: return 0;
    }
}

void I_PreInitGraphics(void)
{
    /* Allocate DOOM framebuffer in PSRAM (320x200 8-bit) */
    doom_fb = heap_caps_calloc(320 * 200, 1, MALLOC_CAP_SPIRAM);
    if (!doom_fb) {
        I_Error("I_PreInitGraphics: Failed to allocate DOOM framebuffer");
    }

    /* Display reports portrait (480x800); we need landscape (800x480).
     * If h_res < v_res, swap to get logical landscape dimensions. */
    if (display_h_res < display_v_res) {
        logical_w = display_v_res;
        logical_h = display_h_res;
    } else {
        logical_w = display_h_res;
        logical_h = display_v_res;
    }

    /* Pre-compute scaling tables for 320x200 -> logical landscape resolution */
    for (int i = 0; i < (int)logical_w && i < 800; i++)
        scale_x[i] = (uint16_t)(i * 320 / logical_w);
    for (int i = 0; i < (int)logical_h && i < 480; i++)
        scale_y[i] = (uint16_t)(i * 200 / logical_h);

    ESP_LOGI(TAG, "Pre-init graphics: %dx%d -> %zux%zu (physical %zux%zu)", 320, 200,
             logical_w, logical_h, display_h_res, display_v_res);
}

void I_CalculateRes(unsigned int width, unsigned int height)
{
    /* Force 320x200 8-bit mode */
    SCREENWIDTH = 320;
    SCREENHEIGHT = 200;
    SCREENPITCH = 320;
}

void I_SetRes(void)
{
    int i;

    I_CalculateRes(SCREENWIDTH, SCREENHEIGHT);

    /* Set first three screens to standard values */
    for (i = 0; i < 3; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENPITCH;
        screens[i].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
        screens[i].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);
    }

    /* Statusbar */
    screens[4].width = SCREENWIDTH;
    screens[4].height = (ST_SCALED_HEIGHT + 1);
    screens[4].byte_pitch = SCREENPITCH;
    screens[4].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
    screens[4].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);

    lprintf(LO_INFO, "I_SetRes: Using resolution %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
}

void I_InitGraphics(void)
{
    static int firsttime = 1;

    if (firsttime) {
        firsttime = 0;

        lprintf(LO_INFO, "I_InitGraphics: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
        I_UpdateVideoMode();
    }
}

void I_UpdateVideoMode(void)
{
    V_InitMode(VID_MODE8);
    V_DestroyUnusedTrueColorPalettes();
    V_FreeScreens();

    I_SetRes();

    /* Point screen 0 at our DOOM framebuffer */
    screens[0].not_on_heap = true;
    screens[0].data = doom_fb;
    screens[0].byte_pitch = SCREENPITCH;
    screens[0].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
    screens[0].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);

    V_AllocScreens();
    R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
}

/* Palette upload: build RGB888 lookup table from PLAYPAL + gamma */
static int newpal = 0;
#define NO_PALETTE_CHANGE 1000

void I_SetPalette(int pal)
{
    newpal = pal;
}

static void I_UploadNewPalette(int pal)
{
    static int cachedgamma = -1;
    static size_t num_pals = 0;

    if (V_GetMode() == VID_MODEGL)
        return;

    int pplump = W_GetNumForName("PLAYPAL");
    int gtlump = (W_CheckNumForName)("GAMMATBL", ns_prboom);
    const byte *palette = W_CacheLumpNum(pplump);
    const byte *gtable = (const byte *)W_CacheLumpNum(gtlump) + 256 * (cachedgamma = usegamma);

    num_pals = W_LumpLength(pplump) / (3 * 256);

    if ((size_t)pal >= num_pals)
        pal = 0;

    const byte *pal_data = palette + pal * 768;

    for (int i = 0; i < 256; i++) {
        uint8_t r = gtable[pal_data[i * 3 + 0]];
        uint8_t g = gtable[pal_data[i * 3 + 1]];
        uint8_t b = gtable[pal_data[i * 3 + 2]];
        rgb_palette[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    W_UnlockLumpNum(pplump);
    W_UnlockLumpNum(gtlump);
}

void I_FinishUpdate(void)
{
    /* Upload palette if changed */
    if (newpal != NO_PALETTE_CHANGE) {
        I_UploadNewPalette(newpal);
        newpal = NO_PALETTE_CHANGE;
    }

    /*
     * Upscale 320x200 8-bit -> logical 800x480 RGB888, then rotate 90° CW
     * into the physical 480x800 PAX framebuffer.
     *
     * Physical (px, py) maps to logical (py, logical_h - 1 - px).
     * PAX_BUF_24_888RGB with little-endian data stores pixels as B,G,R bytes.
     */
    uint8_t *dst = (uint8_t *)pax_buf_get_pixels(&pax_framebuffer);
    int phys_w = (int)display_h_res;    /* 480 */
    int phys_h = (int)display_v_res;    /* 800 */
    int dst_pitch = phys_w * 3;

    for (int py = 0; py < phys_h; py++) {
        /* Logical X = py (physical row -> logical column) */
        uint16_t sx = scale_x[py];
        uint8_t *dst_row = dst + py * dst_pitch;

        for (int px = 0; px < phys_w; px++) {
            /* Logical Y = logical_h - 1 - px (physical col -> logical row, flipped) */
            uint16_t sy = scale_y[logical_h - 1 - px];
            uint32_t color = rgb_palette[doom_fb[sy * 320 + sx]];
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            dst_row[px * 3 + 0] = b;
            dst_row[px * 3 + 1] = g;
            dst_row[px * 3 + 2] = r;
        }
    }

    bsp_display_blit(0, 0, display_h_res, display_v_res,
                     pax_buf_get_pixels(&pax_framebuffer));
}

void I_StartTic(void)
{
    bsp_input_event_t bsp_event;

    handle_hid_events();
    /* A gamepad has nothing to inject into the BSP queue, so it is polled here instead. */
    I_PollJoystick();
    while (xQueueReceive(input_event_queue, &bsp_event, 0) == pdTRUE) {
        if (bsp_event.type == INPUT_EVENT_TYPE_SCANCODE) {
            uint32_t sc = (uint32_t)bsp_event.args_scancode.scancode;
            bool released = (sc & 0x80) != 0;
            int doom_key = tanmatsu_to_doom_key(sc);
            if (doom_key) {
                event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = released ? ev_keyup : ev_keydown;
                ev.data1 = doom_key;
                D_PostEvent(&ev);
            }
        } else if (bsp_event.type == INPUT_EVENT_TYPE_NAVIGATION) {
            /* Handle F1 -> return to launcher */
            if (bsp_event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1 &&
                bsp_event.args_navigation.state) {
                bsp_device_restart_to_launcher();
            }
        }
    }
}

void I_StartFrame(void)
{
}

void I_UpdateNoBlit(void)
{
}

void I_ShutdownGraphics(void)
{
    if (doom_fb) {
        heap_caps_free(doom_fb);
        doom_fb = NULL;
    }
}

int I_ScreenShot(const char *fname)
{
    return -1;
}

int I_GetModeFromString(const char *modestr)
{
    return VID_MODE8;
}
