#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include "solana.h"

typedef struct {
    cl_device_id device;
    cl_context context;
    cl_program program;
    cl_kernel kernel;
    cl_command_queue queue;
    cl_mem result_buf;
    cl_mem key_root_buf;
    cl_mem min_ranges_buf;
    cl_mem max_ranges_buf;
    size_t global_work_size;
    size_t local_work_size;
    uint32_t num_ranges;
} GpuSolana;

typedef struct {
    int platform_idx;
    int device_idx;
    size_t threads;
    size_t local_work_size;
    size_t global_work_size;
    const SolanaMatcher *matcher;
} GpuSolanaOptions;

int gpu_solana_init(GpuSolana *gpu, const GpuSolanaOptions *opts);

int gpu_solana_compute(GpuSolana *gpu, uint8_t *out, const uint8_t *key_root);

void gpu_solana_cleanup(GpuSolana *gpu);

cl_device_id create_device(int platform_idx, int device_idx);
cl_program build_program(cl_context ctx, cl_device_id dev, const char *filename);

#endif
