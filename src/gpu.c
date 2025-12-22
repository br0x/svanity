#include "gpu.h"
#include "opencl_kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cl_device_id create_device(int platform_idx, int device_idx) {
    cl_platform_id platforms[16];
    cl_uint num_platforms;
    cl_device_id dev;
    int err;

    // Get all platforms
    err = clGetPlatformIDs(16, platforms, &num_platforms);
    if (err < 0) {
        fprintf(stderr, "Couldn't identify platforms\n");
        return NULL;
    }

    if (platform_idx >= num_platforms) {
        fprintf(stderr, "Platform index %d out of range (max %d)\n", platform_idx, num_platforms - 1);
        return NULL;
    }

    cl_platform_id platform = platforms[platform_idx];

    // Try GPU first
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &dev, NULL);
    if (err == CL_DEVICE_NOT_FOUND) {
        // Fall back to CPU
        err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &dev, NULL);
    }
    if (err < 0) {
        fprintf(stderr, "Couldn't access any devices\n");
        return NULL;
    }

    return dev;
}

cl_program build_program(cl_context ctx, cl_device_id dev, const char *filename) {
    cl_program program;
    char *program_log;
    size_t program_size, log_size;
    int err;

    // Use embedded kernel source
    (void)filename; // Unused parameter, kept for API compatibility
    const char *program_source = opencl_kernel_source;
    program_size = strlen(program_source);

    // Create program from source
    program = clCreateProgramWithSource(ctx, 1, &program_source, &program_size, &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create the program\n");
        return NULL;
    }

    // Build program
    err = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    if (err < 0) {
        // Get build log
        clGetProgramBuildInfo(program, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        program_log = (char *)malloc(log_size + 1);
        program_log[log_size] = '\0';
        clGetProgramBuildInfo(program, dev, CL_PROGRAM_BUILD_LOG, log_size + 1, program_log, NULL);
        fprintf(stderr, "Build failed:\n%s\n", program_log);
        free(program_log);
        return NULL;
    }

    return program;
}

int gpu_solana_init(GpuSolana *gpu, const GpuSolanaOptions *opts) {
    if (!gpu || !opts || !opts->matcher) {
        return -1;
    }

    cl_int err;

    // Create device
    gpu->device = create_device(opts->platform_idx, opts->device_idx);
    if (!gpu->device) {
        return -1;
    }

    // Print device info
    char device_name[256];
    char vendor_name[256];
    clGetDeviceInfo(gpu->device, CL_DEVICE_NAME, sizeof(device_name), device_name, NULL);
    clGetDeviceInfo(gpu->device, CL_DEVICE_VENDOR, sizeof(vendor_name), vendor_name, NULL);
    fprintf(stderr, "Initializing Solana GPU %s %s\n", vendor_name, device_name);

    // Create context
    gpu->context = clCreateContext(NULL, 1, &gpu->device, NULL, NULL, &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create context\n");
        return -1;
    }

    // Build program
    gpu->program = build_program(gpu->context, gpu->device, "src/opencl/entry.cl");
    if (!gpu->program) {
        clReleaseContext(gpu->context);
        return -1;
    }

    // Create command queue
    gpu->queue = clCreateCommandQueue(gpu->context, gpu->device, 0, &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create command queue\n");
        clReleaseProgram(gpu->program);
        clReleaseContext(gpu->context);
        return -1;
    }

    // Create kernel
    gpu->kernel = clCreateKernel(gpu->program, "generate_solana_pubkey", &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create kernel\n");
        clReleaseCommandQueue(gpu->queue);
        clReleaseProgram(gpu->program);
        clReleaseContext(gpu->context);
        return -1;
    }

    // Setup ranges
    gpu->num_ranges = opts->matcher->num_ranges;
    size_t ranges_size = gpu->num_ranges * SOLANA_PUBKEY_SIZE;

    // Create buffers
    gpu->result_buf = clCreateBuffer(gpu->context, CL_MEM_WRITE_ONLY, sizeof(uint64_t), NULL, &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create result buffer\n");
        goto cleanup;
    }

    gpu->key_root_buf = clCreateBuffer(gpu->context, CL_MEM_READ_ONLY, SOLANA_PRIVKEY_SIZE, NULL, &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create key_root buffer\n");
        goto cleanup;
    }

    gpu->min_ranges_buf = clCreateBuffer(gpu->context, CL_MEM_READ_ONLY, ranges_size, NULL, &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create min_ranges buffer\n");
        goto cleanup;
    }

    gpu->max_ranges_buf = clCreateBuffer(gpu->context, CL_MEM_READ_ONLY, ranges_size, NULL, &err);
    if (err < 0) {
        fprintf(stderr, "Couldn't create max_ranges buffer\n");
        goto cleanup;
    }

    // Write range data
    uint8_t *min_data = malloc(ranges_size);
    uint8_t *max_data = malloc(ranges_size);

    for (size_t i = 0; i < gpu->num_ranges; i++) {
        memcpy(min_data + i * SOLANA_PUBKEY_SIZE, opts->matcher->ranges[i].min, SOLANA_PUBKEY_SIZE);
        memcpy(max_data + i * SOLANA_PUBKEY_SIZE, opts->matcher->ranges[i].max, SOLANA_PUBKEY_SIZE);
    }

    clEnqueueWriteBuffer(gpu->queue, gpu->min_ranges_buf, CL_TRUE, 0, ranges_size, min_data, 0, NULL, NULL);
    clEnqueueWriteBuffer(gpu->queue, gpu->max_ranges_buf, CL_TRUE, 0, ranges_size, max_data, 0, NULL, NULL);

    free(min_data);
    free(max_data);

    // Initialize result to sentinel value
    uint64_t sentinel = (uint64_t)-1;
    clEnqueueWriteBuffer(gpu->queue, gpu->result_buf, CL_TRUE, 0, sizeof(uint64_t), &sentinel, 0, NULL, NULL);

    // Set kernel arguments
    err = clSetKernelArg(gpu->kernel, 0, sizeof(cl_mem), &gpu->result_buf);
    err |= clSetKernelArg(gpu->kernel, 1, sizeof(cl_mem), &gpu->key_root_buf);
    err |= clSetKernelArg(gpu->kernel, 2, sizeof(cl_mem), &gpu->min_ranges_buf);
    err |= clSetKernelArg(gpu->kernel, 3, sizeof(cl_mem), &gpu->max_ranges_buf);
    err |= clSetKernelArg(gpu->kernel, 4, sizeof(uint32_t), &gpu->num_ranges);

    if (err < 0) {
        fprintf(stderr, "Couldn't set kernel arguments\n");
        goto cleanup;
    }

    // Set work sizes
    gpu->global_work_size = opts->global_work_size > 0 ? opts->global_work_size : opts->threads;
    gpu->local_work_size = opts->local_work_size;

    return 0;

cleanup:
    if (gpu->result_buf) clReleaseMemObject(gpu->result_buf);
    if (gpu->key_root_buf) clReleaseMemObject(gpu->key_root_buf);
    if (gpu->min_ranges_buf) clReleaseMemObject(gpu->min_ranges_buf);
    if (gpu->max_ranges_buf) clReleaseMemObject(gpu->max_ranges_buf);
    clReleaseKernel(gpu->kernel);
    clReleaseCommandQueue(gpu->queue);
    clReleaseProgram(gpu->program);
    clReleaseContext(gpu->context);
    return -1;
}

int gpu_solana_compute(GpuSolana *gpu, uint8_t *out, const uint8_t *key_root) {
    cl_int err;

    // Write key root to device
    err = clEnqueueWriteBuffer(gpu->queue, gpu->key_root_buf, CL_TRUE, 0, SOLANA_PRIVKEY_SIZE, key_root, 0, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "Couldn't write key_root buffer\n");
        return -1;
    }

    // Execute kernel
    err = clEnqueueNDRangeKernel(gpu->queue, gpu->kernel, 1, NULL, &gpu->global_work_size,
                                  gpu->local_work_size > 0 ? &gpu->local_work_size : NULL, 0, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "Couldn't enqueue kernel: %d\n", err);
        return -1;
    }

    // Wait for completion
    clFinish(gpu->queue);

    // Read result
    uint64_t global_id;
    err = clEnqueueReadBuffer(gpu->queue, gpu->result_buf, CL_TRUE, 0, sizeof(uint64_t), &global_id, 0, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "Couldn't read result buffer\n");
        return -1;
    }

    // Check if we found a match
    bool success = (global_id != (uint64_t)-1);

    if (success) {
        // Reset result buffer
        uint64_t sentinel = (uint64_t)-1;
        clEnqueueWriteBuffer(gpu->queue, gpu->result_buf, CL_TRUE, 0, sizeof(uint64_t), &sentinel, 0, NULL, NULL);

        // Reconstruct the private key
        memcpy(out, key_root, 29);
        out[29] = (global_id >> 16) & 0xFF;
        out[30] = (global_id >> 8) & 0xFF;
        out[31] = global_id & 0xFF;

        return 1; // Found
    }

    return 0; // Not found
}

void gpu_solana_cleanup(GpuSolana *gpu) {
    if (!gpu) return;

    if (gpu->result_buf) clReleaseMemObject(gpu->result_buf);
    if (gpu->key_root_buf) clReleaseMemObject(gpu->key_root_buf);
    if (gpu->min_ranges_buf) clReleaseMemObject(gpu->min_ranges_buf);
    if (gpu->max_ranges_buf) clReleaseMemObject(gpu->max_ranges_buf);
    if (gpu->kernel) clReleaseKernel(gpu->kernel);
    if (gpu->queue) clReleaseCommandQueue(gpu->queue);
    if (gpu->program) clReleaseProgram(gpu->program);
    if (gpu->context) clReleaseContext(gpu->context);

    memset(gpu, 0, sizeof(GpuSolana));
}
