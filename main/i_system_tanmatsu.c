/* i_system_tanmatsu.c -- System interface for Tanmatsu (replaces SDL/i_system.c) */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "lprintf.h"
#include "doomtype.h"
#include "doomdef.h"
#include "m_fixed.h"
#include "r_fps.h"
#include "i_system.h"
#include "m_argv.h"

static const char *TAG __attribute__((unused)) = "i_system";

static unsigned int start_displaytime;
static unsigned int displaytime;
static boolean InDisplay = false;

boolean I_StartDisplay(void)
{
    if (InDisplay)
        return false;
    start_displaytime = (unsigned int)(esp_timer_get_time() / 1000);
    InDisplay = true;
    return true;
}

void I_EndDisplay(void)
{
    displaytime = (unsigned int)(esp_timer_get_time() / 1000) - start_displaytime;
    InDisplay = false;
}

int ms_to_next_tick;

int I_GetTime_RealTime(void)
{
    int t = (int)(esp_timer_get_time() / 1000); /* milliseconds */
    int i = t * (TICRATE / 5) / 200;
    ms_to_next_tick = (i + 1) * 200 / (TICRATE / 5) - t;
    if (ms_to_next_tick > 1000 / TICRATE || ms_to_next_tick < 1)
        ms_to_next_tick = 1;
    return i;
}

fixed_t I_GetTimeFrac(void)
{
    unsigned long now;
    fixed_t frac;

    now = (unsigned long)(esp_timer_get_time() / 1000);

    if (tic_vars.step == 0)
        return FRACUNIT;
    else {
        frac = (fixed_t)((now - tic_vars.start + displaytime) * FRACUNIT / tic_vars.step);
        if (frac < 0)
            frac = 0;
        if (frac > FRACUNIT)
            frac = FRACUNIT;
        return frac;
    }
}

void I_GetTime_SaveMS(void)
{
    if (!movement_smooth)
        return;

    tic_vars.start = (unsigned int)(esp_timer_get_time() / 1000);
    tic_vars.next = (unsigned int)((tic_vars.start * tic_vars.msec + 1.0f) / tic_vars.msec);
    tic_vars.step = tic_vars.next - tic_vars.start;
}

unsigned long I_GetRandomTimeSeed(void)
{
    return (unsigned long)(esp_timer_get_time() & 0xFFFFFFFF);
}

void I_uSleep(unsigned long usecs)
{
    TickType_t ticks = pdMS_TO_TICKS(usecs / 1000);
    if (ticks < 1)
        ticks = 1;
    vTaskDelay(ticks);
}

const char *I_GetVersionString(char *buf, size_t sz)
{
    snprintf(buf, sz, "%s v%s", PACKAGE_NAME, VERSION);
    return buf;
}

const char *I_SigString(char *buf, size_t sz, int signum)
{
    snprintf(buf, sz, "signal %d", signum);
    return buf;
}

void I_Read(int fd, void *vbuf, size_t sz)
{
    unsigned char *buf = vbuf;

    while (sz) {
        int rc = read(fd, buf, sz);
        if (rc <= 0) {
            I_Error("I_Read: read failed: %s", rc ? strerror(errno) : "EOF");
        }
        sz -= rc;
        buf += rc;
    }
}

int I_Filelength(int handle)
{
    struct stat fileinfo;
    if (fstat(handle, &fileinfo) == -1)
        I_Error("I_Filelength: %s", strerror(errno));
    return fileinfo.st_size;
}

const char *I_DoomExeDir(void)
{
    return TADOOM_BASE_PATH;
}

boolean HasTrailingSlash(const char *dn)
{
    return (dn[strlen(dn) - 1] == '/');
}

char *I_FindFile(const char *wfname, const char *ext)
{
    /* First try the path as-is (handles absolute paths from -iwad) */
    if (!access(wfname, F_OK)) {
        char *p = malloc(strlen(wfname) + 1);
        strcpy(p, wfname);
        lprintf(LO_INFO, " found %s\n", p);
        return p;
    }
    {
        char *p = malloc(strlen(wfname) + strlen(ext) + 1);
        sprintf(p, "%s%s", wfname, ext);
        if (!access(p, F_OK)) {
            lprintf(LO_INFO, " found %s\n", p);
            return p;
        }
        free(p);
    }

    /* Search only in TADOOM_BASE_PATH */
    static const struct {
        const char *dir;
        const char *sub;
        const char *env;
        const char *(*func)(void);
    } search[] = {
        {TADOOM_BASE_PATH, NULL, NULL, NULL},
        {NULL, NULL, NULL, I_DoomExeDir},
    };

    size_t pl = strlen(wfname) + strlen(ext) + 4;

    for (int i = 0; i < (int)(sizeof(search) / sizeof(*search)); i++) {
        char *p;
        const char *d = NULL;
        const char *s = NULL;

        if (search[i].env) {
            d = getenv(search[i].env);
            if (!d) continue;
        } else if (search[i].func)
            d = search[i].func();
        else
            d = search[i].dir;
        s = search[i].sub;

        p = malloc((d ? strlen(d) : 0) + (s ? strlen(s) : 0) + pl);
        sprintf(p, "%s%s%s%s%s", d ? d : "", (d && !HasTrailingSlash(d)) ? "/" : "",
                s ? s : "", (s && !HasTrailingSlash(s)) ? "/" : "",
                wfname);

        if (access(p, F_OK))
            strcat(p, ext);
        if (!access(p, F_OK)) {
            lprintf(LO_INFO, " found %s\n", p);
            return p;
        }
        free(p);
    }
    return NULL;
}

void I_SetAffinityMask(void)
{
    /* No-op on ESP32 */
}
