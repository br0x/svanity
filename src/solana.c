#include "solana.h"
#include "base58.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sodium.h>
#include <gmp.h>
#include <stdint.h>
#include <stddef.h>

void secret_to_pubkey_solana(const uint8_t secret[SOLANA_PRIVKEY_SIZE], uint8_t pubkey[SOLANA_PUBKEY_SIZE]) {
    // Step 1: Hash the secret key with SHA-512
    uint8_t hash[64];
    crypto_hash_sha512(hash, secret, SOLANA_PRIVKEY_SIZE);

    // Step 2: Clamp the hash (first 32 bytes)
    uint8_t clamped[32];
    memcpy(clamped, hash, 32);
    clamped[0] &= 248;    // Clear lowest 3 bits
    clamped[31] &= 127;   // Clear highest bit
    clamped[31] |= 64;    // Set second-highest bit

    // Step 3: Scalar multiply by base point (ED25519)
    crypto_scalarmult_ed25519_base_noclamp(pubkey, clamped);
}

void pubkey_to_base58(const uint8_t pubkey[SOLANA_PUBKEY_SIZE], char *out) {
    size_t out_len = base58_encode(out, pubkey, SOLANA_PUBKEY_SIZE);
    out[out_len] = '\0';
}

static int decode_base58_to_bytes(const char *s, uint8_t *out) {
    uint8_t buf[64];
    ssize_t len = base58_decode(buf, s);

    if (len != SOLANA_PUBKEY_SIZE) {
        return -1;
    }

    memcpy(out, buf, SOLANA_PUBKEY_SIZE);
    return 0;
}

int prefix_to_all_ranges(const char *prefix, SolanaMatcher *matcher) {
    if (prefix == NULL || matcher == NULL) {
        return -1;
    }

    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0) {
        // Empty prefix - match everything
        matcher->num_ranges = 1;
        matcher->ranges = malloc(sizeof(PubkeyRange));
        memset(matcher->ranges[0].min, 0, SOLANA_PUBKEY_SIZE);
        memset(matcher->ranges[0].max, 255, SOLANA_PUBKEY_SIZE);
        return 0;
    }

    // Allocate space for potential ranges (max is ~13 different lengths)
    PubkeyRange *temp_ranges = malloc(sizeof(PubkeyRange) * 20);
    size_t num_ranges = 0;

    // Try different address lengths from 32 to 44 characters
    for (size_t target_len = 32; target_len <= 44; target_len++) {
        if (target_len < prefix_len) {
            continue;
        }

        size_t padding_len = target_len - prefix_len;

        // Create min string: prefix + '1's
        char min_str[64];
        snprintf(min_str, sizeof(min_str), "%s", prefix);
        for (size_t i = 0; i < padding_len; i++) {
            strcat(min_str, "1");
        }

        // Create max string: prefix + 'z's
        char max_str[64];
        snprintf(max_str, sizeof(max_str), "%s", prefix);
        for (size_t i = 0; i < padding_len; i++) {
            strcat(max_str, "z");
        }

        // Try to decode both
        uint8_t min_bytes[SOLANA_PUBKEY_SIZE];
        uint8_t max_bytes[SOLANA_PUBKEY_SIZE];

        if (decode_base58_to_bytes(min_str, min_bytes) == 0 &&
            decode_base58_to_bytes(max_str, max_bytes) == 0) {
            memcpy(temp_ranges[num_ranges].min, min_bytes, SOLANA_PUBKEY_SIZE);
            memcpy(temp_ranges[num_ranges].max, max_bytes, SOLANA_PUBKEY_SIZE);
            num_ranges++;
        }
    }

    if (num_ranges == 0) {
        free(temp_ranges);
        return -1;
    }

    // Allocate final ranges array
    matcher->ranges = malloc(sizeof(PubkeyRange) * num_ranges);
    memcpy(matcher->ranges, temp_ranges, sizeof(PubkeyRange) * num_ranges);
    matcher->num_ranges = num_ranges;

    free(temp_ranges);
    return 0;
}

bool solana_matcher_matches(const SolanaMatcher *matcher, const uint8_t pubkey[SOLANA_PUBKEY_SIZE]) {
    for (size_t i = 0; i < matcher->num_ranges; i++) {
        const uint8_t *min = matcher->ranges[i].min;
        const uint8_t *max = matcher->ranges[i].max;

        // Check if pubkey is within range [min, max]
        if (memcmp(pubkey, min, SOLANA_PUBKEY_SIZE) >= 0 &&
            memcmp(pubkey, max, SOLANA_PUBKEY_SIZE) <= 0) {
            return true;
        }
    }
    return false;
}

void solana_matcher_free(SolanaMatcher *matcher) {
    if (matcher && matcher->ranges) {
        free(matcher->ranges);
        matcher->ranges = NULL;
        matcher->num_ranges = 0;
    }
}

uint64_t estimate_attempts(const char *prefix) {
    if (prefix == NULL) {
        return 1;
    }

    size_t len = strlen(prefix);
    if (len == 0) {
        return 1;
    }

    uint64_t attempts = 1;
    for (size_t i = 0; i < len; i++) {
        if (attempts > UINT64_MAX / 58) {
            return UINT64_MAX;  // Overflow protection
        }
        attempts *= 58;
    }

    return attempts;
}

// Helper to calculate n = (P * 2^256) / S using GMP long math
static uint64_t calculate_n_gmp_internal(mpz_t S, uint64_t P_fixed) {
    if (mpz_sgn(S) == 0) return 0;

    mpz_t P, numerator, quotient;
    mpz_inits(P, numerator, quotient, NULL);

    // 1. Set P as the unsigned fixed-point value
    mpz_set_ui(P, P_fixed);

    // 2. numerator = P * 2^256 (equivalent to P << 256)
    mpz_mul_2exp(numerator, P, 192);

    // 3. quotient = numerator / S
    mpz_tdiv_q(quotient, numerator, S);

    // 4. Convert result to uint64_t
    uint64_t result;
    // if (mpz_fits_ulong_p(quotient)) {
        result = (uint64_t)mpz_get_ui(quotient);
    // } else {
    //     // If S is so small that the quotient exceeds 64 bits
    //     result = 0xFFFFFFFFFFFFFFFFULL;
    // }

    mpz_clears(P, numerator, quotient, NULL);
    return result;
}

int get_estimates_gmp(SolanaMatcher *matcher, ConfidenceEstimates *ce) {
    mpz_t S, temp_min, temp_max, diff;
    mpz_inits(S, temp_min, temp_max, diff, NULL);
    mpz_set_ui(S, 0);

    // Calculate Total Size S = sum(max - min)
    for (size_t i = 0; i < matcher->num_ranges; i++) {
        // mpz_import: 32 bytes, order -1 (little-endian), 
        // size 1 (byte-wise), endian 0 (native), nails 0
        mpz_import(temp_min, 32, 1, 1, 0, 0, matcher->ranges[i].min);
        mpz_import(temp_max, 32, 1, 1, 0, 0, matcher->ranges[i].max);

        mpz_sub(diff, temp_max, temp_min);
        mpz_add_ui(diff, diff, 1); // +1 for inclusive range
        mpz_add(S, S, diff);
    }

    // Calculate probabilities using the single S value
    // Note: 2^64 is 100%, so P values are fractions of 2^64
    ce->p50  = calculate_n_gmp_internal(S, 0x8000000000000000ULL); // 50%
    ce->p90  = calculate_n_gmp_internal(S, 0xE666666666666666ULL); // 90%
    ce->p99  = calculate_n_gmp_internal(S, 0xfd70a3d70a3d70a3ULL); // 99%

    // Clean up
    mpz_clears(S, temp_min, temp_max, diff, NULL);
    return 0;
}
