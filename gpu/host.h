/* The guest's OpenGL ES on the host's GPU (gpu/host.c): the aoi_proc gpu hook. */
#ifndef AOI_GPU_HOST_H
#define AOI_GPU_HOST_H

#include <stdint.h>

struct aoi_proc;

/* p->gpu: AOI_SYS_GL (core/gpu.h). */
uint64_t aoi_gpu_call(void *ctx, struct aoi_proc *p, uint64_t op, uint64_t args);

/* 1 if the host has a GPU for the guest (its EGL display came up). */
int aoi_gpu_available(void);

/* The process using it ended: its contexts, surfaces and sync objects go. */
void aoi_gpu_end(void);

#endif
