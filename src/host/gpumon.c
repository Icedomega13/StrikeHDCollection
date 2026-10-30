/* Graphics card load, temperature and power. See gpumon.h. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include "gpumon.h"

/* ---- NVIDIA: NVML, the subset used here (stable C ABI) ---- */
typedef void *nvml_dev;
typedef struct { unsigned gpu, memory; } nvml_util;
static struct {
    int tried, ok;
    SDL_SharedObject *lib;
    nvml_dev dev;
    int (*Init)(void);
    int (*GetHandleByIndex)(unsigned, nvml_dev *);
    int (*GetUtilizationRates)(nvml_dev, nvml_util *);
    int (*GetTemperature)(nvml_dev, int, unsigned *);
    int (*GetPowerUsage)(nvml_dev, unsigned *);
    int (*GetFanSpeed)(nvml_dev, unsigned *);
    int (*GetClockInfo)(nvml_dev, int, unsigned *);
} nv;

static int nvml_open(void)
{
    if (nv.tried) return nv.ok;
    nv.tried = 1;
#ifdef _WIN32
    nv.lib = SDL_LoadObject("nvml.dll");
#else
    nv.lib = SDL_LoadObject("libnvidia-ml.so.1");
#endif
    if (!nv.lib) return 0;
#define F(field, name) *(void **)&nv.field = (void *)SDL_LoadFunction(nv.lib, name)
    F(Init, "nvmlInit_v2");
    F(GetHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
    F(GetUtilizationRates, "nvmlDeviceGetUtilizationRates");
    F(GetTemperature, "nvmlDeviceGetTemperature");
    F(GetPowerUsage, "nvmlDeviceGetPowerUsage");
    F(GetFanSpeed, "nvmlDeviceGetFanSpeed");
    F(GetClockInfo, "nvmlDeviceGetClockInfo");
#undef F
    if (!nv.Init || !nv.GetHandleByIndex || nv.Init() != 0 || nv.GetHandleByIndex(0, &nv.dev) != 0) return 0;
    nv.ok = 1;
    return 1;
}

/* ---- AMD: amdgpu sysfs ---- */
static int read_int(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return (int)v;
}

static char amd_dir[128], amd_hwmon[192];
static int amd_open(void)
{
    static int tried, ok;
    if (tried) return ok;
    tried = 1;
#ifndef _WIN32
    for (int c = 0; c < 4 && !ok; c++) {
        char p[160];
        SDL_snprintf(p, sizeof p, "/sys/class/drm/card%d/device/gpu_busy_percent", c);
        if (read_int(p) < 0) continue;
        SDL_snprintf(amd_dir, sizeof amd_dir, "/sys/class/drm/card%d/device", c);
        for (int h = 0; h < 16; h++) {
            SDL_snprintf(amd_hwmon, sizeof amd_hwmon, "%s/hwmon/hwmon%d", amd_dir, h);
            SDL_snprintf(p, sizeof p, "%s/temp1_input", amd_hwmon);
            if (read_int(p) >= 0) break;
            amd_hwmon[0] = 0;
        }
        ok = 1;
    }
#endif
    return ok;
}

static void gpumon_poll(gpu_stat *s)
{
    *s = (gpu_stat){ NULL, -1, -1, -1, -1, -1 };
    unsigned v;
    if (nvml_open()) {
        nvml_util u;
        s->source = "NVML";
        if (nv.GetUtilizationRates && nv.GetUtilizationRates(nv.dev, &u) == 0) s->load = (int)u.gpu;
        if (nv.GetTemperature && nv.GetTemperature(nv.dev, 0, &v) == 0) s->temp = (int)v;        /* NVML_TEMPERATURE_GPU */
        if (nv.GetPowerUsage && nv.GetPowerUsage(nv.dev, &v) == 0) s->power = (int)(v / 1000);  /* mW */
        if (nv.GetFanSpeed && nv.GetFanSpeed(nv.dev, &v) == 0) s->fan = (int)v;
        if (nv.GetClockInfo && nv.GetClockInfo(nv.dev, 0, &v) == 0) s->clock = (int)v;          /* NVML_CLOCK_GRAPHICS */
        return;
    }
    if (amd_open()) {
        char p[256];
        s->source = "amdgpu";
        SDL_snprintf(p, sizeof p, "%s/gpu_busy_percent", amd_dir);
        s->load = read_int(p);
        if (amd_hwmon[0]) {
            SDL_snprintf(p, sizeof p, "%s/temp1_input", amd_hwmon);
            int t = read_int(p);
            s->temp = t >= 0 ? t / 1000 : -1;
            SDL_snprintf(p, sizeof p, "%s/power1_average", amd_hwmon);
            int w = read_int(p);
            if (w < 0) { SDL_snprintf(p, sizeof p, "%s/power1_input", amd_hwmon); w = read_int(p); }
            s->power = w >= 0 ? w / 1000000 : -1;
            SDL_snprintf(p, sizeof p, "%s/pwm1", amd_hwmon);
            int f = read_int(p);
            s->fan = f >= 0 ? f * 100 / 255 : -1;
            SDL_snprintf(p, sizeof p, "%s/freq1_input", amd_hwmon);
            int hz = read_int(p);
            s->clock = hz >= 0 ? hz / 1000000 : -1;
        }
    }
}

/* ---- the polling thread ---- */
static SDL_Mutex *lock;
static gpu_stat latest = { NULL, -1, -1, -1, -1, -1 };

static int SDLCALL poll_thread(void *ud)
{
    (void)ud;
    for (;;) {
        gpu_stat s;
        gpumon_poll(&s);
        SDL_LockMutex(lock);
        latest = s;
        SDL_UnlockMutex(lock);
        SDL_Delay(1000);
    }
    return 0;
}

void gpumon_get(gpu_stat *s)
{
    static SDL_Thread *th;
    if (!lock && (lock = SDL_CreateMutex()) && (th = SDL_CreateThread(poll_thread, "gpumon", NULL)))
        SDL_DetachThread(th);
    if (!lock) { *s = latest; return; }
    SDL_LockMutex(lock);
    *s = latest;
    SDL_UnlockMutex(lock);
}
