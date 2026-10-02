/* The guest's OpenGL ES on the host's GPU (gpu/host.c): the aoi_proc gpu hook. */
#ifndef AOI_GPU_HOST_H
#define AOI_GPU_HOST_H

#include <stdint.h>

struct aoi_proc;

/* p->gpu: AOI_SYS_GL (core/gpu.h). */
uint64_t aoi_gpu_call(void *ctx, struct aoi_proc *p, uint64_t op, uint64_t args);

#endif
