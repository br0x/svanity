// For CLOCK_MONOTONIC and usleep
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sodium.h>
#include "vanity.h"

// CPU worker thread
void* cpu_worker_thread(void *arg) {
    ThreadParams *params = (ThreadParams *)arg;

    uint8_t key[SOLANA_PRIVKEY_SIZE];
    uint8_t pubkey[SOLANA_PUBKEY_SIZE];
    char address[64];

    // Initialize random key
    randombytes_buf(key, SOLANA_PRIVKEY_SIZE);

    while (1) {
        // Generate public key from private key
        secret_to_pubkey_solana(key, pubkey);

        // Fast byte-level check (no base58 conversion needed)
        if (solana_matcher_matches(params->matcher, pubkey)) {
            // Verify it's a real match by checking the base58 address
            pubkey_to_base58(pubkey, address);

            if (strncmp(address, params->prefix, strlen(params->prefix)) == 0) {
                if (params->output_progress) {
                    fprintf(stderr, "\n");
                }

                // Print result (to stdout for simple output, stderr for verbose)
                if (params->simple_output) {
                    for (int i = 0; i < SOLANA_PRIVKEY_SIZE; i++) {
                        printf("%02X", key[i]);
                    }
                    printf(" %s\n", address);
                    fflush(stdout);
                } else {
                    fprintf(stderr, "Found matching account!\nPrivate Key: ");
                    for (int i = 0; i < SOLANA_PRIVKEY_SIZE; i++) {
                        fprintf(stderr, "%02X", key[i]);
                    }
                    fprintf(stderr, "\nAddress:     %s\n", address);
                    fflush(stderr);
                }

                // Check if we've reached the limit
                size_t found = atomic_fetch_add(params->found_n, 1) + 1;
                if (params->limit != 0 && found >= params->limit) {
                    exit(0);
                }
            }
        }

        // Update attempts counter
        if (params->output_progress) {
            atomic_fetch_add(params->attempts, 1);
        }

        // Increment key (treat as 256-bit little-endian integer)
        for (int i = SOLANA_PRIVKEY_SIZE - 1; i >= 0; i--) {
            key[i]++;
            if (key[i] != 0) {
                break;
            }
        }
    }

    return NULL;
}

// GPU worker thread
void* gpu_worker_thread(void *arg) {
    GpuThreadParams *params = (GpuThreadParams *)arg;

    uint8_t key_base[SOLANA_PRIVKEY_SIZE];
    uint8_t found_key[SOLANA_PRIVKEY_SIZE];
    uint8_t pubkey[SOLANA_PUBKEY_SIZE];
    char address[64];

    while (1) {
        // Generate random key base
        randombytes_buf(key_base, SOLANA_PRIVKEY_SIZE);

        // Run GPU computation
        int result = gpu_solana_compute(params->gpu, found_key, key_base);

        // Update attempts counter
        if (params->output_progress) {
            atomic_fetch_add(params->attempts, params->gpu_threads);
        }

        if (result <= 0) {
            continue; // No match found
        }

        // Verify the solution
        secret_to_pubkey_solana(found_key, pubkey);
        pubkey_to_base58(pubkey, address);

        if (strncmp(address, params->prefix, strlen(params->prefix)) == 0) {
            if (params->output_progress) {
                fprintf(stderr, "\n");
            }

            // Print result (to stdout for simple output, stderr for verbose)
            if (params->simple_output) {
                for (int i = 0; i < SOLANA_PRIVKEY_SIZE; i++) {
                    printf("%02X", found_key[i]);
                }
                printf(" %s\n", address);
                fflush(stdout);
            } else {
                fprintf(stderr, "Found matching account!\nPrivate Key: ");
                for (int i = 0; i < SOLANA_PRIVKEY_SIZE; i++) {
                    fprintf(stderr, "%02X", found_key[i]);
                }
                fprintf(stderr, "\nAddress:     %s\n", address);
                fflush(stderr);
            }

            // Check if we've reached the limit
            size_t found = atomic_fetch_add(params->found_n, 1) + 1;
            if (params->limit != 0 && found >= params->limit) {
                exit(0);
            }
        } else {
            fprintf(stderr, "GPU returned non-matching solution: ");
            for (int i = 0; i < SOLANA_PRIVKEY_SIZE; i++) {
                fprintf(stderr, "%02X", found_key[i]);
            }
            fprintf(stderr, "\n");
        }
    }

    return NULL;
}

// Progress reporting thread
void* progress_thread(void *arg) {
    atomic_size_t *attempts = (atomic_size_t *)arg;

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        usleep(250000); // Sleep 250ms

        size_t attempts_val = atomic_load(attempts);
        clock_gettime(CLOCK_MONOTONIC, &now);

        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;

        // Avoid division by zero on first iteration
        double keys_per_second = (elapsed > 0) ? (attempts_val / elapsed) : 0.0;

        fprintf(stderr, "\rTried %zu keys (%.1f keys/s)", attempts_val, keys_per_second);
        fflush(stderr);
    }

    return NULL;
}
