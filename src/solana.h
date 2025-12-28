#ifndef SOLANA_H
#define SOLANA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SOLANA_PUBKEY_SIZE 32
#define SOLANA_PRIVKEY_SIZE 32

typedef struct {
    uint8_t min[SOLANA_PUBKEY_SIZE];
    uint8_t max[SOLANA_PUBKEY_SIZE];
} PubkeyRange;

typedef struct {
    uint64_t p50;
    uint64_t p90;
    uint64_t p99;
} ConfidenceEstimates;

typedef struct {
    PubkeyRange *ranges;
    size_t num_ranges;
} SolanaMatcher;

void secret_to_pubkey_solana(const uint8_t secret[SOLANA_PRIVKEY_SIZE], uint8_t pubkey[SOLANA_PUBKEY_SIZE]);

void pubkey_to_base58(const uint8_t pubkey[SOLANA_PUBKEY_SIZE], char *out);

int prefix_to_all_ranges(const char *prefix, SolanaMatcher *matcher);

bool solana_matcher_matches(const SolanaMatcher *matcher, const uint8_t pubkey[SOLANA_PUBKEY_SIZE]);

void solana_matcher_free(SolanaMatcher *matcher);

uint64_t estimate_attempts(const char *prefix);

int estimate_attempts_confidence(const char *prefix, const SolanaMatcher *matcher, ConfidenceEstimates *estimates);

int get_estimates_gmp(SolanaMatcher *matcher, ConfidenceEstimates *ce);

#endif
