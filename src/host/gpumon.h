/*
 * Graphics card load, temperature and power, for Show FPS: Diagnostics.
 * NVIDIA through NVML (the driver's libnvidia-ml / nvml.dll, loaded at run
 * time if present), AMD through amdgpu's sysfs files on Linux. Nothing is
 * needed at build time; on other cards every value stays unknown (-1).
 */
#ifndef DS_GPUMON_H
#define DS_GPUMON_H

typedef struct {
    const char *source;        /* "NVML", "amdgpu" or NULL when nothing is available */
    int load;                  /* % of the time the GPU was busy */
    int temp;                  /* degrees C */
    int power;                 /* W */
    int fan;                   /* % */
    int clock;                 /* graphics clock, MHz */
} gpu_stat;

/* The latest values. The first call starts a thread that reads them once a
 * second (opening NVML can take seconds, a reading milliseconds: neither
 * belongs on the game's thread); until its first reading, all unknown. */
void gpumon_get(gpu_stat *s);

#endif
