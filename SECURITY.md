# Vanylith Security Architecture

## Randomness

Vanylith calls Windows `BCryptGenRandom(nullptr, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG)` for every new 32-byte task base key. It does not use `rand`, time, thread identifiers, GPU identifiers, or a deterministic PRNG as entropy. Generated values are rejected until `libsecp256k1` confirms `1 <= key < n`.

## Private-key lifecycle

The task base key exists only in native C++/CUDA memory. CPU and CUDA workers receive disjoint offsets from one atomic `SearchSpaceAllocator`; no backend owns a private counter that could overlap another worker. CPU workers retain their current batch and index across pause/resume, while CUDA pauses only after an already-launched bounded batch reaches its synchronization boundary. Temporary host candidate buffers, host base-key copies, CUDA result buffers, and textual private-key records are overwritten before release when the platform permits it.

Memory clearing is best effort. Compiler, allocator, driver, VRAM/cache remanence, crash-dump, and operating-system behavior cannot provide an absolute forensic-erasure guarantee. In particular, CUDA kernel parameters and hardware caches are managed by the NVIDIA driver and cannot be synchronously proven erased by the application.

## Address derivation

The CPU reference performs:

1. `libsecp256k1` uncompressed public-key derivation.
2. Removal of the `0x04` serialization byte.
3. Keccak-256 over the 64-byte `X || Y` value.
4. The last 20 digest bytes prefixed with TRON mainnet byte `0x41`.
5. Double Windows CNG SHA-256 and the first four checksum bytes.
6. Base58 encoding.

Keccak-256 uses legacy Keccak domain padding `0x01`; it is deliberately not SHA3-256 (`0x06`). Tests cover standard Keccak vectors and public TRON vectors.

The CUDA backend independently performs secp256k1 fixed-generator multiplication, legacy Keccak-256, double SHA-256, Base58Check, and pattern matching on the GPU. Its correctness tests compare known vectors, sequential and boundary scalars, and every supported pattern mode against the CPU reference on an RTX 5070 (`sm_120`).

## Candidate verification

Every match is passed back to the controller as a C++ private-key buffer. The controller independently re-derives the address with `libsecp256k1` and Windows cryptography, requires exact equality with the GPU-reported address, and re-runs pattern matching. Only a passing result may be saved or published. This boundary is mandatory for CPU and CUDA candidates and remains required for any future OpenCL backend.

## Result files

C++ writes `results/task-*/wallets.txt` directly. A protected DACL grants full access to SYSTEM and the file owner. Each record is flushed before the public result is marked `savedLocally: true`. The Web server has no route for result-file reading or arbitrary filesystem access.

## Web boundary

The embedded Dashboard receives address, time, device, backend, task state, statistics, CPU verification, and save status. API serialization has no private-key, mnemonic, seed, or secret field.

The server:

- explicitly binds the IPv4 loopback address `127.0.0.1`, never `0.0.0.0`;
- uses `SO_EXCLUSIVEADDRUSE`;
- accepts only loopback peers;
- requires `Host: 127.0.0.1:8787`;
- requires `Origin: http://127.0.0.1:8787` and JSON content type for POST;
- rejects chunked request bodies and limits headers and bodies to 16 KiB;
- sends CSP, frame-denial, MIME-sniffing, referrer, and no-store headers.

SSE publishes a public snapshot approximately every 750 ms. Closing or reloading the page has no effect on the engine. Closing the native Vanylith window stops any active task, shuts down the loopback server, and exits the process.

## Networking and process behavior

At runtime, Winsock is used only to accept and respond on the bound loopback listener. Vanylith has no native outbound `connect` path and no native HTTP client. The default WebView2 shell is restricted to the literal local Dashboard origin, denies web permission requests, blocks new windows, and exposes no host objects or native private-key API. `ShellExecuteW` is used only when the developer explicitly requests legacy `--browser` mode. Build-time CMake `FetchContent` accesses the pinned upstream `libsecp256k1` Git repository and the hash-pinned Microsoft WebView2 SDK package; that build behavior is not present in the executable.

## Logging

Logs contain startup, backend initialization, local-listener status, desktop-shell status, browser-launch failure, and shutdown. They never include private keys, candidate values, random bytes, seeds, or mnemonics.

## Dependency

`bitcoin-core/libsecp256k1` v0.8.0 is pinned through signed annotated tag object `18f07c42218765cd46148d74d9fe575795f56dce`, which peels to commit `6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d`. It is used under the MIT license for secp256k1 key validation and public-key derivation. All unrelated optional modules are disabled. Microsoft Edge WebView2 SDK 1.0.4191.47 is used under its BSD-3-Clause license; only its loader is statically linked, while the browser engine comes from Microsoft's Evergreen Runtime.

## Reporting

Do not include private keys, wallet files, entropy, or seed material in a vulnerability report. Provide a minimal reproduction using disposable test keys only.
