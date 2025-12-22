#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sodium.h>
#include "argtable3.h"
#include "solana.h"
#include "gpu.h"
#include "vanity.h"

int main(int argc, char *argv[]) {
    // Initialize libsodium
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }
    // 1. Declare the argtable structures
    struct arg_lit  *help    = arg_lit0("h", "help", "display this help and exit");
    struct arg_lit  *version = arg_lit0(NULL, "version", "display version info and exit");

    // Required positional argument: prefix
    struct arg_str  *prefix  = arg_str1(NULL, NULL, "PREFIX", "The prefix for the address");

    // Optional arguments with defaults
    struct arg_int  *threads = arg_int0("t", "threads", "N", "The number of threads to use [default: number of cores minus one]");
    struct arg_lit  *gpu     = arg_lit0("g", "gpu", "Enable use of the GPU through OpenCL");
    struct arg_int  *limit   = arg_int0("l", "limit", "N", "Generate N addresses, then exit (0 for infinite)");
    struct arg_int  *gpu_threads = arg_int0(NULL, "gpu-threads", "N", "The number of GPU threads to use");
    
    // Optional arguments for advanced users
    struct arg_int  *gpu_local_work_size = arg_int0(NULL, "gpu-local-work-size", "N", "The GPU local work size. For advanced users only.");
    struct arg_int  *gpu_global_work_size = arg_int0(NULL, "gpu-global-work-size", "N", "The GPU global work size. For advanced users only.");
    
    // Optional flags
    struct arg_lit  *no_progress = arg_lit0(NULL, "no-progress", "Disable progress output");
    struct arg_lit  *simple_output = arg_lit0(NULL, "simple-output", "Output found keys in the form \"[key] [address]\"");

    // Optional arguments for GPU device selection
    struct arg_int  *gpu_platform = arg_int0(NULL, "gpu-platform", "INDEX", "The GPU platform to use");
    struct arg_int  *gpu_device = arg_int0(NULL, "gpu-device", "INDEX", "The GPU device to use");

    // The mandatory end-of-table marker
    struct arg_end  *end     = arg_end(20);

    // 2. Define the argtable array
    void *argtable[] = {
        prefix, threads, gpu, limit, gpu_threads, gpu_local_work_size,
        gpu_global_work_size, no_progress, simple_output, gpu_platform,
        gpu_device, help, version, end
    };

    const char *progname = "solana-vanity"; // argv[0]

    // Set default values (argtable3 requires explicit defaults)
    limit->ival[0] = 1;
    gpu_threads->ival[0] = 1048576;
    gpu_platform->ival[0] = 0;
    gpu_device->ival[0] = 0;
    // threads default is dynamic, so we'd set it after parsing if not present.

    // 3. Parse the command line
    int nerrors = arg_parse(argc, argv, argtable);

    // 4. Handle help and version
    if (help->count > 0) {
        printf("Usage: %s\n", progname);
        arg_print_syntax(stdout, argtable, "\n");
        printf("%s\n\n", "Generate SOLANA addresses with a given prefix");
        arg_print_glossary(stdout, argtable, "  %-25s %s\n");
        return 0;
    }
    if (version->count > 0) {
        printf("%s version %s\n", progname, "1.2.3"); // Replace 1.2.3 with actual version
        return 0;
    }

    // 5. Handle errors
    if (nerrors > 0) {
        arg_print_errors(stdout, end, progname);
        printf("Try '%s --help' for more information.\n", progname);
        return 1;
    }

    // 6. Access parsed values
    const char *prefix_str = prefix->sval[0];
    size_t limit_val = limit->ival[0];
    bool use_gpu = (gpu->count > 0);
    bool output_progress = (no_progress->count == 0);
    bool simple_output_flag = (simple_output->count > 0);

    int num_threads = threads->count > 0 ? threads->ival[0] : (sysconf(_SC_NPROCESSORS_ONLN) - 1);
    if (num_threads < 1) num_threads = 1;

    // Create matcher from prefix
    SolanaMatcher matcher;
    if (prefix_to_all_ranges(prefix_str, &matcher) != 0) {
        fprintf(stderr, "Failed to create matcher for prefix: %s\n", prefix_str);
        arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return 1;
    }

    // Setup shared state
    atomic_size_t found_n = 0;
    atomic_size_t attempts = 0;

    // Print search info BEFORE starting threads
    if (!simple_output_flag) {
        fprintf(stderr, "Searching for Solana addresses starting with: %s\n", prefix_str);
        fprintf(stderr, "Using fast byte-level range matching\n");
        fprintf(stderr, "Found %zu range(s) for this prefix:\n\n", matcher.num_ranges);

        // Print estimated attempts with confidence intervals
        ConfidenceEstimates estimates;
        if (estimate_attempts_confidence(prefix_str, &matcher, &estimates) == 0) {
            fprintf(stderr, "Estimated total attempts:\n");
            fprintf(stderr, "  %lu (50%%), %lu (90%%), %lu (95%%)\n\n",
                    estimates.p50, estimates.p90, estimates.p95);
        } else {
            // Fallback to simple estimate
            uint64_t estimated = estimate_attempts(prefix_str);
            if (estimated == UINT64_MAX) {
                fprintf(stderr, "Estimated total attempts: >18 quintillion (overflow)\n\n");
            } else {
                fprintf(stderr, "Estimated total attempts: %lu\n\n", estimated);
            }
        }

        // Print each range
        for (size_t i = 0; i < matcher.num_ranges; i++) {
            char min_addr[64], max_addr[64];
            pubkey_to_base58(matcher.ranges[i].min, min_addr);
            pubkey_to_base58(matcher.ranges[i].max, max_addr);

            fprintf(stderr, "  Range %zu:\n", i + 1);
            fprintf(stderr, "    Min: %s, len: %zu (0x", min_addr, strlen(min_addr));
            for (int j = 0; j < SOLANA_PUBKEY_SIZE; j++) {
                fprintf(stderr, "%02X", matcher.ranges[i].min[j]);
            }
            fprintf(stderr, ")\n    Max: %s, len: %zu (0x", max_addr, strlen(max_addr));
            for (int j = 0; j < SOLANA_PUBKEY_SIZE; j++) {
                fprintf(stderr, "%02X", matcher.ranges[i].max[j]);
            }
            fprintf(stderr, ")\n\n");
        }
        fprintf(stderr, "\n");
        fflush(stderr); // Ensure all output is printed before threads start
    }

    // Allocate and prepare CPU thread parameters (but don't start threads yet)
    pthread_t *cpu_threads = malloc(sizeof(pthread_t) * num_threads);
    ThreadParams *cpu_params = malloc(sizeof(ThreadParams) * num_threads);

    for (int i = 0; i < num_threads; i++) {
        cpu_params[i].limit = limit_val;
        cpu_params[i].found_n = &found_n;
        cpu_params[i].output_progress = output_progress;
        cpu_params[i].attempts = &attempts;
        cpu_params[i].simple_output = simple_output_flag;
        cpu_params[i].matcher = &matcher;
        cpu_params[i].prefix = prefix_str;
    }

    // Prepare GPU thread if requested (but don't start yet)
    pthread_t gpu_thread;
    GpuThreadParams gpu_params;
    GpuSolana gpu_ctx;
    bool gpu_ready = false;

    if (use_gpu) {
        GpuSolanaOptions gpu_opts = {
            .platform_idx = gpu_platform->ival[0],
            .device_idx = gpu_device->ival[0],
            .threads = gpu_threads->ival[0],
            .local_work_size = gpu_local_work_size->count > 0 ? gpu_local_work_size->ival[0] : 0,
            .global_work_size = gpu_global_work_size->count > 0 ? gpu_global_work_size->ival[0] : 0,
            .matcher = &matcher
        };

        if (gpu_solana_init(&gpu_ctx, &gpu_opts) == 0) {
            gpu_params.gpu = &gpu_ctx;
            gpu_params.limit = limit_val;
            gpu_params.found_n = &found_n;
            gpu_params.output_progress = output_progress;
            gpu_params.attempts = &attempts;
            gpu_params.simple_output = simple_output_flag;
            gpu_params.prefix = prefix_str;
            gpu_params.gpu_threads = gpu_threads->ival[0];
            gpu_ready = true;
        } else {
            fprintf(stderr, "Warning: Failed to initialize GPU, continuing with CPU only\n");
            use_gpu = false;
        }
    }

    // NOW start all threads (after everything is printed)
    // Flush both stdout and stderr to ensure all output appears in order
    fflush(stdout);
    fflush(stderr);

    // Start progress thread first
    pthread_t progress_thd;
    if (output_progress) {
        pthread_create(&progress_thd, NULL, progress_thread, &attempts);
    }

    // Start CPU worker threads
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&cpu_threads[i], NULL, cpu_worker_thread, &cpu_params[i]);
    }

    // Start GPU worker thread
    if (gpu_ready) {
        pthread_create(&gpu_thread, NULL, gpu_worker_thread, &gpu_params);
    }

    // Wait for threads
    for (int i = 0; i < num_threads; i++) {
        pthread_join(cpu_threads[i], NULL);
    }

    if (gpu_ready) {
        pthread_join(gpu_thread, NULL);
        gpu_solana_cleanup(&gpu_ctx);
    }

    // Cleanup
    free(cpu_threads);
    free(cpu_params);
    solana_matcher_free(&matcher);
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));

    fprintf(stderr, "\nAll threads completed\n");
    return 0;
}
