# CUDA portability record

## Build strategy

- Build host toolkit: CUDA 13.3.x (the release configuration rejects another minor series)
- Runtime: `CUDA::cudart_static` / `CUDA_RUNTIME_LIBRARY Static`
- Host runtime: static MSVC `/MT`
- Native cubins: `sm_80`, `sm_86`, `sm_89`, `sm_120`
- PTX fallback: `compute_120`
- Driver-owned libraries: CUDA Driver and dynamically loaded NVML
- Redistributed NVIDIA DLLs: none

The architecture list comes from CUDA 13.3.73 `nvcc --list-gpu-code`. Product
mapping comes from NVIDIA's official [CUDA GPU compute capability table](https://developer.nvidia.com/cuda-gpus).

## Driver compatibility

NVIDIA documents Release 580 as the CUDA 13.x minor-compatibility floor. The
program calls the CUDA Runtime/Driver version APIs before device discovery and
requires CUDA Driver API 13.0 or newer. Known target GPUs use native cubins, so
they do not depend on PTX JIT for normal execution. A future architecture that
uses `compute_120` PTX requires a driver new enough to understand the CUDA 13.3
PTX version.

CUDA 12.8 is a sensible future second artifact for Ampere/Ada cloud hosts on
R525-R579 drivers. It is intentionally deferred: it needs a separate clean
build, dependency audit, correctness suite, and real-GPU stability matrix.

## Release gates

1. Clean `windows-x64-release` configure and Release build.
2. Full CTest and CUDA fixed-vector correctness tests.
3. Short GPU-only benchmark and Dashboard lifecycle test.
4. `dumpbin /dependents` audit with no CUDA Toolkit, MSVC runtime, or NVML DLL.
5. Package hash manifest and ZIP hash.
6. Extracted ZIP launch with CUDA environment variables removed and a PATH
   containing only Windows system directories.

The clean-environment launch does not prove every possible clean machine state
by itself. Combined with static-link configuration, import-table audit, and a
successful CUDA device enumeration outside Toolkit PATH, it demonstrates that
the executable does not depend on `nvcc`, Toolkit development files, or a
Toolkit runtime DLL. Final confirmation should be performed on a clean system where the
CUDA Toolkit has never been installed.
