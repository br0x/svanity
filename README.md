# C Implementation of Solana Vanity Address Generator

## Overview

This is Solana vanity address generator in pure C, with OpenCL GPU acceleration support.

## Architecture

### Core Components

1. **[src/main.c](src/main.c)** - Main entry point
   - Command-line argument parsing using argtable3
   - Thread management (CPU and GPU workers)
   - Initialization and cleanup

2. **[src/solana.c](src/solana.c)** / **[src/solana.h](src/solana.h)** - Solana key operations
   - `secret_to_pubkey_solana()` - ED25519 key derivation using libsodium
   - `pubkey_to_base58()` - Convert public key to Solana address
   - `prefix_to_all_ranges()` - Convert Base58 prefix to byte ranges
   - `solana_matcher_matches()` - Fast byte-level range matching

3. **[src/gpu.c](src/gpu.c)** / **[src/gpu.h](src/gpu.h)** - OpenCL GPU management
   - `gpu_solana_init()` - Initialize OpenCL device, context, buffers
   - `gpu_solana_compute()` - Execute GPU kernel for batch key generation
   - `gpu_solana_cleanup()` - Free OpenCL resources

4. **[src/vanity.c](src/vanity.c)** / **[src/vanity.h](src/vanity.h)** - Worker threads
   - `cpu_worker_thread()` - CPU-based key generation and matching
   - `gpu_worker_thread()` - GPU-based key generation and matching
   - `progress_thread()` - Real-time progress reporting

5. **[src/opencl/entry.cl](src/opencl/entry.cl)** - OpenCL kernel
   - Full ED25519 scalar multiplication on GPU
   - Batch processing of private keys
   - Range-based matching on GPU

6. **[src/base58.c](src/base58.c)** / **[src/base58.h](src/base58.h)** - Base58 encoding
   - Solana address encoding/decoding

## Key Design Decisions

### 1. Using libsodium for ED25519
For ED25519, we use libsodium's optimized implementation:
- `crypto_hash_sha512()` - SHA-512 hashing
- `crypto_scalarmult_ed25519_base_noclamp()` - Scalar multiplication
- `randombytes_buf()` - Cryptographically secure random number generation

### 2. Range-Based Matching
- Convert Base58 prefix to byte ranges (min, max)
- Fast byte comparison: `memcmp(pubkey, min) >= 0 && memcmp(pubkey, max) <= 0`
- Only convert to Base58 when byte range matches
- Dramatically reduces expensive Base58 conversions

### 3. Multi-Threading
- **CPU threads**: Each thread increments through private keys independently
- **GPU thread**: Processes batches of keys in parallel on GPU
- **Progress thread**: Reports statistics without blocking workers
- Uses atomic operations (`atomic_size_t`) for thread-safe counters

### 4. GPU Architecture
- Private key split: base (29 bytes) + variable (3 bytes)
- GPU kernel processes 2^24 variations of each base
- Result passed back as global_id, reconstructed to full private key

## Algorithm Flow

### CPU Worker Thread
```
1. Initialize with random private key
2. Loop forever:
   a. Generate public key from private key (ED25519)
   b. Check if pubkey falls in byte ranges (fast)
   c. If match, convert to Base58 and verify prefix
   d. If verified, print result and check limit
   e. Increment private key (treat as 256-bit integer)
   f. Update attempts counter
```

### GPU Worker Thread
```
1. Loop forever:
   a. Generate random 32-byte base
   b. Send base to GPU
   c. GPU tests base + [0..2^24] variations
   d. GPU returns global_id if match found
   e. Reconstruct full private key from global_id
   f. Verify match by converting to Base58
   g. If verified, print result and check limit
   h. Update attempts counter
```

## Performance Characteristics

### CPU Performance
- Uses libsodium's highly optimized ED25519 implementation
- Multi-core scaling: N-1 cores used by default
- Memory efficient: Each thread uses minimal stack space
- Lock-free: Atomic counters for coordination

### GPU Performance
- Parallel execution of 1M+ key generations
- Full ED25519 implementation runs on GPU
- Batch processing reduces host-device transfers
- Range matching on GPU reduces data transfer

## Building

```bash
# Install dependencies
sudo apt install libsodium-dev ocl-icd-opencl-dev cmake build-essential

# Build
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Usage Examples

```bash
# CPU only (uses all cores - 1)
./svanity ABC

# CPU with 4 threads
./svanity -t 4 test

# GPU acceleration
./svanity -g ABC

# Generate 5 matching addresses
./svanity -l 5 ABC

# Simple output for scripting
./svanity --simple-output ABC

# GPU with custom settings
./svanity -g --gpu-threads 2097152 --gpu-platform 0 --gpu-device 0 ABC
```

## Future Improvements

1. **Performance**
   - Profile and optimize hot paths
   - SIMD optimization for range checking
   - Better GPU work size auto-tuning

2. **Features**
   - Suffix matching (not just prefix)
   - Case-insensitive matching
   - Multiple prefix patterns
   - Statistics tracking (total attempts, time estimates)

## Credits
Kernel code for ED25519 and SHA-512 was taken from [SolVanityCL](https://github.com/WincerChan/SolVanityCL).
