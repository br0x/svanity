#ifndef VANITY_H
#define VANITY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

#include "solana.h"
#include "gpu.h"

typedef struct {
    size_t limit;
    atomic_size_t *found_n;
    bool output_progress;
    atomic_size_t *attempts;
    bool simple_output;
    const SolanaMatcher *matcher;
    const char *prefix;
} ThreadParams;

typedef struct {
    GpuSolana *gpu;
    size_t limit;
    atomic_size_t *found_n;
    bool output_progress;
    atomic_size_t *attempts;
    bool simple_output;
    const char *prefix;
    size_t gpu_threads;
} GpuThreadParams;

void* cpu_worker_thread(void *arg);
void* gpu_worker_thread(void *arg);
void* progress_thread(void *arg);

#endif
