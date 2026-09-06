/* main.c -- TaDOOM entry point for Tanmatsu (ESP32-P4) */

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"

/* PrBoom headers */
#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"
#include "m_fixed.h"
#include "i_system.h"
#include "i_video.h"
#include "z_zone.h"
#include "lprintf.h"
#include "m_random.h"
#include "doomstat.h"
#include "g_game.h"
#include "m_misc.h"
#include "i_sound.h"
#include "i_main.h"
#include "r_fps.h"

#include "hidhost.h"

static const char *TAG = "tadoom";

/* Display state - shared with i_video_tanmatsu.c */
size_t display_h_res = 0;
size_t display_v_res = 0;
bsp_display_color_format_t display_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB;
bsp_display_endianness_t display_data_endian = BSP_DISPLAY_ENDIAN_LITTLE;
pax_buf_t pax_framebuffer = {0};
QueueHandle_t input_event_queue = NULL;

/* PrBoom globals */
int realtic_clock_rate = 100;
static int_64_t I_GetTime_Scale = 1 << 24;
unsigned int endoom_mode = 0;

static int I_GetTime_Scaled(void)
{
    return (int)((int_64_t)I_GetTime_RealTime() * I_GetTime_Scale >> 24);
}

static int I_GetTime_FastDemo(void)
{
    static int fasttic;
    return fasttic++;
}

static int I_GetTime_Error(void)
{
    I_Error("I_GetTime_Error: GetTime() used before initialization");
    return 0;
}

int (*I_GetTime)(void) = I_GetTime_Error;

void I_Init(void)
{
    hid_init();
    if (fastdemo)
        I_GetTime = I_GetTime_FastDemo;
    else if (realtic_clock_rate != 100) {
        I_GetTime_Scale = ((int_64_t)realtic_clock_rate << 24) / 100;
        I_GetTime = I_GetTime_Scaled;
    } else
        I_GetTime = I_GetTime_RealTime;

    if (!(nomusicparm && nosfxparm))
        I_InitSound();

    R_InitInterpolation();
}

void I_SafeExit(int rc)
{
    ESP_LOGI(TAG, "I_SafeExit called with rc=%d, returning to launcher", rc);
    bsp_device_restart_to_launcher();
}

static void I_Quit(void) __attribute__((unused));
static void I_Quit(void)
{
    if (demorecording)
        G_CheckDemoStatus();
    M_SaveDefaults();
}

static void doom_task(void *arg)
{
    ESP_LOGI(TAG, "Starting D_DoomMain...");
    D_DoomMain();
    ESP_LOGI(TAG, "D_DoomMain returned (unexpected)");
    vTaskDelay(portMAX_DELAY);
}

void app_main(void)
{
    esp_err_t res;

    /* GPIO ISR service */
    gpio_install_isr_service(0);

    /* NVS */
    res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(res));
        return;
    }

    /* BSP initialization with RGB888 display */
    const bsp_configuration_t bsp_config = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
            .num_fbs = 1,
        },
    };
    res = bsp_device_initialize(&bsp_config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %s", esp_err_to_name(res));
        return;
    }

    /* Get display parameters */
    res = bsp_display_get_parameters(&display_h_res, &display_v_res,
                                     &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Display params failed: %s", esp_err_to_name(res));
        return;
    }
    ESP_LOGI(TAG, "Display: %ux%u", (unsigned)display_h_res, (unsigned)display_v_res);

    /* Input queue */
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    /* Initialize PAX framebuffer with cache-aligned, DMA-safe memory.
     * DMA2D reads this buffer for display; without alignment, cache
     * sync operations can corrupt adjacent heap allocations. */
    size_t fb_size = display_h_res * display_v_res * 3; /* RGB888 */
    void *fb_mem = heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!fb_mem) {
        ESP_LOGE(TAG, "Failed to allocate DMA-safe framebuffer (%u bytes)", (unsigned)fb_size);
        return;
    }
    memset(fb_mem, 0, fb_size);
    pax_buf_init(&pax_framebuffer, fb_mem, display_h_res, display_v_res, PAX_BUF_24_888RGB);
    pax_buf_reversed(&pax_framebuffer, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);

    /* FIX2-BEGIN: Splash blits disabled to avoid DMA2D activity during init.
     * DMA2D transfers overlap with heavy heap allocation in R_Init/P_SetupLevel,
     * risking cache coherency corruption. Re-enable once root cause is confirmed
     * fixed by the DMA-safe framebuffer allocation (Fix 1) above.
     *
     * Original splash screen code:
     *   pax_background(&pax_framebuffer, 0xFF000000);
     *   pax_draw_text(&pax_framebuffer, 0xFFFFFFFF, pax_font_sky_mono, 24, 10, 10, "TaDOOM v0.1.0");
     *   pax_draw_text(&pax_framebuffer, 0xFFCCCCCC, pax_font_sky_mono, 16, 10, 40, "Mounting SD card...");
     *   bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&pax_framebuffer));
     * FIX2-END */
    ESP_LOGI(TAG, "Mounting SD card...");

    /* Mount SD card */
    res = sdcard_init();
    if (res != ESP_OK) {
        /* FIX2-BEGIN: Keep error blit - user needs to see this, and no heap
         * activity follows since we halt with vTaskDelay(portMAX_DELAY). */
        pax_background(&pax_framebuffer, 0xFFFF0000);
        pax_draw_text(&pax_framebuffer, 0xFFFFFFFF, pax_font_sky_mono, 16, 10, 10,
                      "SD card mount failed!");
        pax_draw_text(&pax_framebuffer, 0xFFFFFFFF, pax_font_sky_mono, 16, 10, 30,
                      "Please insert SD card with WAD files");
        pax_draw_text(&pax_framebuffer, 0xFFFFFFFF, pax_font_sky_mono, 16, 10, 50,
                      "at /sd/apps/at.cavac.tadoom/ and reboot.");
        bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&pax_framebuffer));
        /* FIX2-END */
        vTaskDelay(portMAX_DELAY);
        return;
    }

    /* FIX2-BEGIN: "Starting DOOM engine" splash disabled (see above).
     *
     * Original code:
     *   pax_background(&pax_framebuffer, 0xFF000000);
     *   pax_draw_text(&pax_framebuffer, 0xFFFFFFFF, pax_font_sky_mono, 24, 10, 10, "TaDOOM v0.1.0");
     *   pax_draw_text(&pax_framebuffer, 0xFFCCCCCC, pax_font_sky_mono, 16, 10, 40, "Starting DOOM engine...");
     *   bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&pax_framebuffer));
     * FIX2-END */
    ESP_LOGI(TAG, "Starting DOOM engine...");

    /* Set up argc/argv for PrBoom */
    static const char *doom_argv[] = {"tadoom", "-iwad", TADOOM_BASE_PATH "/doom1.wad", NULL};
    myargc = 3;
    myargv = doom_argv;

    /* Initialize PrBoom memory system */
    Z_Init();

    /* Pre-init graphics (stores references for later) */
    I_PreInitGraphics();

    /* Launch DOOM on a FreeRTOS task with large stack (in PSRAM, TCB in internal RAM) */
    StaticTask_t *task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    StackType_t *task_stack = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
    if (!task_buf || !task_stack) {
        ESP_LOGE(TAG, "Failed to allocate doom task memory from PSRAM");
        return;
    }
    xTaskCreateStaticPinnedToCore(doom_task, "doom", 65536, NULL, 1,
                                  task_stack, task_buf, 0);
}
