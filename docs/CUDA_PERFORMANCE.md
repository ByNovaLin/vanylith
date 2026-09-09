# CUDA performance validation

Validated on 2026-09-03 with an NVIDIA GeForce RTX 5070, driver 610.62,
CUDA Toolkit 13.3.73, `sm_120`, and a `RelWithDebInfo` build. This is a
development benchmark, not a final Release package.

## Method

- Each pattern ran for 25 seconds; the first 5 seconds were discarded.
- CPU search was disabled for GPU-only pattern measurements.
- Keys/s came from the real backend counters exposed by `/api/status`.
- Utilization, temperature, power, and VRAM came from `nvidia-smi`/NVML.
- Every candidate still executed secp256k1, Keccak-256, double SHA-256,
  Base58Check, and pattern matching. Search ranges and CPU verification were
  unchanged.

The benchmark can be repeated with `scripts/benchmark-cuda.ps1`.

## CUDA Phase 1 final configuration (2026-09-06)

CUDA Phase 1 is frozen at 32 point-progression/batch-inversion threads per
block, 64 candidates per inversion group, and a 1,048,576-candidate search
batch. The final RTX 3090 parameter screen found no alternative that reached
the 3% retention threshold:

| Configuration | Group-kernel threads/block | Inversion group | Reported MKeys/s | Change vs. T32-G64 |
|---|---:|---:|---:|---:|
| **T32-G64** | **32** | **64** | **185.945** | **baseline / retained** |
| T64-G64 | 64 | 64 | 183.649 | -1.23% |
| T32-G32 | 32 | 32 | 156.152 | -16.02% |
| T32-G128 | 32 | 128 | 162.010 | -12.87% |
| T64-G32 | 64 | 32 | 151.631 | -18.45% |
| T64-G128 | 64 | 128 | 157.344 | -15.38% |

The final native-device performance is 185.945 MKeys/s averaged across the
four Literal/Repeat Prefix/Suffix modes on RTX 3090 / `sm_86`, and 206.331
MKeys/s in the no-code-change confirmation run on RTX 5070 / `sm_120`.

The RTX 3090 Phase 1 performance progression was:

| Stage | Checkpoint/change | MKeys/s | Gain from previous stage |
|---|---|---:|---:|
| Early CUDA baseline | Monolithic pipeline | about 42 | - |
| Split pipeline | `099bcd958e1738a383ebe9d7e8b4654ff86825b4` | about 63 | about 1.5x |
| Optimized field multiplication | `78fc917bab5bacdea421e6a4c4550852b014f962` | **185.945** | about 2.96x vs. measured 62.84 baseline |

No further arithmetic or launch tuning is included in this checkpoint.
Progression register pressure, an `sm_86`-specific carry chain, detailed SASS
and Nsight profiling, and workspace/global-memory optimization are explicitly
deferred to a possible future CUDA phase.

## Ampere field-arithmetic checkpoint (2026-09-05)

An RTX 3090 hardware run of the split-pipeline checkpoint measured about
63.77 MKeys/s for Literal Suffix and 62.84 MKeys/s averaged over 22 seconds
(approximately 1.384 billion checked keys). Because another CUDA vanity
implementation reached about 348.955 MKeys/s on the same host, this result
isolated a remaining implementation bottleneck rather than a host-level GPU
problem.

The second-stage change replaces the generic 17-limb schoolbook multiply and
repeated high-limb folding in secp256k1 field arithmetic with a fixed-column
Comba multiply and bounded reduction using `2^256 = 2^32 + 977 (mod p)`.
Kernel boundaries, group width, block sizes, workspace layout, address/hash
logic, and launch configuration are unchanged for this A/B. The new reduction
was checked against independent arbitrary-precision modular arithmetic for
100,000 random input pairs. The CUDA suite also independently compares every
GPU address in four 129-candidate, multi-group batches with CPU
libsecp256k1 derivation.

CUDA 13.3 Release resource usage before and after this arithmetic-only change:

| Architecture | Kernel | Before registers/stack | After registers/stack |
|---|---|---:|---:|
| sm_86 | Point progression | 130 / 304 B | 136 / 32 B |
| sm_86 | Batch inversion | 95 / 72 B | 80 / 0 B |
| sm_86 | Address/match | 118 / 784 B | 118 / 784 B |
| sm_120 | Point progression | 168 / 168 B | 176 / 168 B |
| sm_120 | Batch inversion | 96 / 0 B | 90 / 0 B |
| sm_120 | Address/match | 118 / 288 B | 118 / 288 B |

On the local RTX 5070, the unchanged four-mode 20-second benchmark with a
5-second warmup improved from 164.589 MKeys/s to 210.287 MKeys/s mean
(+27.8%):

| Pattern | Split pipeline MKeys/s | Field arithmetic MKeys/s | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 165.336 | 210.487 | 97.8% | 66.9 C | 181.40 W | 2,846 MiB |
| Literal Suffix | 166.004 | 210.991 | 97.5% | 64.1 C | 180.42 W | 2,817 MiB |
| Repeat Prefix | 165.042 | 210.125 | 97.7% | 62.5 C | 179.51 W | 2,881 MiB |
| Repeat Suffix | 161.975 | 209.545 | 97.7% | 61.8 C | 178.34 W | 2,805 MiB |
| **Mean** | **164.589** | **210.287** | **97.7%** | **63.8 C** | **179.92 W** | **2,837 MiB** |

The native RTX 3090 / `sm_86` A/B of implementation checkpoint
`78fc917bab5bacdea421e6a4c4550852b014f962` confirmed that the arithmetic
change resolves the dominant Ampere regression:

| Pattern | Split-pipeline baseline MKeys/s | Field arithmetic MKeys/s |
|---|---:|---:|
| Literal Prefix | - | 186.149 |
| Literal Suffix | about 63.77 | 183.379 |
| Repeat Prefix | - | 187.330 |
| Repeat Suffix | - | 186.922 |
| **Four-mode mean** | **62.84 overall measured baseline** | **185.945** |

This is approximately 2.96x the same-machine 62.84 MKeys/s baseline, a 196%
increase. The RTX 3090 run reported approximately 94-95% GPU utilization,
348.5 W of a 350 W power limit, 65.9 C maximum temperature, about 987 MiB of
VRAM, and no CUDA error. The first-stage 100 MKeys/s target is therefore
complete.

A later no-code-change confirmation run on the RTX 5070 produced 204.315,
206.056, 209.708, and 205.246 MKeys/s for Literal Prefix, Literal Suffix,
Repeat Prefix, and Repeat Suffix respectively. The 206.331 MKeys/s mean is
1.88% below the earlier 210.287 MKeys/s short run, within short-run operating
variance and with no regression relative to the 164.589 MKeys/s split
checkpoint. GPU utilization remained 97.7-98.0%, and all four cases completed
without a CUDA error.

An Nsight Systems timing sample after the change attributed 59.1% of CUDA
kernel time to point progression, 13.9% to batch inversion, and 26.9% to
address/hash/matching. The corresponding split-pipeline sample was 61.6%,
16.7%, and 21.7%; maximum progression and inversion calls fell from 3.941 ms
and 1.181 ms to 3.268 ms and 0.780 ms. The address kernel remained essentially
unchanged in absolute time.

## Split-pipeline checkpoint (2026-09-05)

The portable RC exposed the monolithic kernel's architecture risk on an RTX
3090: its native `sm_86` cubin used 250 registers per thread and a 17,920-byte
thread stack. Each thread retained 128 Jacobian points, 128 prefix products,
and all Keccak/SHA-256/Base58 state. This made the thread-local arrays local
memory and limited the register-bound Ampere launch to roughly eight active
warps per SM.

The search is now split into ordered kernels for point progression, batch
inversion, and address generation/matching. Points, prefix products, and Z
inverses use an explicitly transposed global workspace so a warp accesses
adjacent groups at a common position. The hash kernel assigns one thread per
candidate. Scalar validation uses a first/last fast path but retains the
per-candidate fallback at the secp256k1 order or 64-bit offset boundary. Every
candidate still completes the exact Keccak-256, double SHA-256, Base58Check,
and pattern pipeline.

`cuobjdump --dump-resource-usage` for the final CUDA 13.3 Release fat binary:

| Architecture | Kernel | Registers/thread | Stack/thread |
|---|---|---:|---:|
| sm_86 | Point progression | 130 | 304 B |
| sm_86 | Batch inversion | 95 | 72 B |
| sm_86 | Address/match | 118 | 784 B |
| sm_120 | Point progression | 168 | 168 B |
| sm_120 | Batch inversion | 96 | 0 B |
| sm_120 | Address/match | 118 | 288 B |

The largest `sm_86` stack frame is 95.6% smaller than the RC monolithic frame;
the register-bound ceiling is approximately 16 active warps per SM rather
than eight. The fat binary contains native `sm_80`, `sm_86`, `sm_89`, and
`sm_120` cubins plus `compute_120` PTX, so Ampere does not use PTX JIT.

Group-width A/B testing on the RTX 5070 selected 64 candidates per inversion
group. Widths 32, 64, 128, and 256 averaged approximately 117.36, 155.06,
118.74, and 79.42 MKeys/s respectively before the later boundary and memory
layout improvements. A 64-thread group-kernel block also lost slightly to the
32-thread block, so the final launch uses 32 threads for progression/inversion
and 128 threads for address generation.

Final CUDA 13.3 portable Release results on the RTX 5070 (`sm_120`), with 20
seconds per pattern and the first 5 seconds discarded:

| Pattern | RC baseline Keys/s | Split pipeline Keys/s |
|---|---:|---:|
| Literal Prefix | 97,607,933.3 | 165,335,933.3 |
| Literal Suffix | 95,911,320.0 | 166,003,933.3 |
| Repeat Prefix | 96,700,440.0 | 165,042,066.7 |
| Repeat Suffix | 97,994,240.0 | 161,975,466.7 |
| **Mean** | **97,053,483.3** | **164,589,350.0** |

This is a 69.6% mean gain over the same-session RC baseline and a 51.5% gain
over the historical 108.64 MKeys/s checkpoint. The final suite passed 36/36
tests. An RTX 3090 was not installed in the validation host, so the `sm_86`
resource fix is compile-verified but its post-fix MKeys/s still requires a
3090 hardware run.

## Scheduling-checkpoint RTX 5070 results

| Pattern | Baseline Keys/s | Optimized Keys/s | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 768.1 | 818,939.4 | 98.2% | 52.5 C | 82.92 W | 1,582 MiB |
| Literal Suffix | 767.3 | 785,032.0 | 98.6% | 55.4 C | 83.35 W | 1,603 MiB |
| Repeat Prefix | 789.6 | 767,482.6 | 98.6% | 57.9 C | 84.66 W | 1,624 MiB |
| Repeat Suffix | 768.1 | 767,874.4 | 98.4% | 60.3 C | 86.43 W | 1,732 MiB |
| **Mean** | **773.3** | **784,832.1** | **98.5%** | **56.5 C** | **84.34 W** | **1,635 MiB** |

The mean improvement is 1,014.96x, or 101,395.91% over the checkpoint baseline.
The small differences among pattern modes show that matching is not the
dominant cost.

## Point-progression checkpoint

The first structural optimization after the scheduling baseline groups eight
contiguous private keys per CUDA thread. The first key still uses the audited
full Jacobian scalar multiplication. The other seven public points use the
identity `(k + 1)G = kG + G`, and all eight Jacobian Z coordinates share one
Montgomery batch inversion. The kernel batch grew from 8,192 to 65,536 keys so
8,192 independent groups remain in flight.

Measured with the same 25-second cases and 5-second warmup:

| Pattern | Scheduling baseline Keys/s | Point progression Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 741,479.4 | 5,558,876.5 | 7.50x | 98.4% | 60.0 C | 89.81 W | 1,719 MiB |
| Literal Suffix | 699,990.2 | 5,829,812.0 | 8.33x | 98.6% | 61.8 C | 91.55 W | 1,728 MiB |
| Repeat Prefix | 748,133.0 | 5,873,825.0 | 7.85x | 98.6% | 57.5 C | 89.97 W | 1,718 MiB |
| Repeat Suffix | 751,452.8 | 5,627,305.5 | 7.49x | 98.4% | 54.6 C | 88.23 W | 1,718 MiB |
| **Mean** | **735,263.9** | **5,722,454.8** | **7.78x** | **98.5%** | **58.5 C** | **89.89 W** | **1,720.8 MiB** |

`cuobjdump --dump-resource-usage` reports 244 registers per thread and a
1,504-byte stack for this kernel. The larger local point/prefix arrays trade
occupancy for much lower arithmetic per key; reducing this footprint is the
next tuning target.

## Fixed-base window checkpoint

A 64-position, 4-bit fixed-base table replaces the group-start double-and-add
loop (256 point doublings plus about 128 conditional additions) with at most 64
mixed Jacobian/affine additions. The table contains only public multiples of G.
It is generated with CPU libsecp256k1 during CUDA initialization and copied to
61,440 bytes of read-only GPU global storage; secret base scalars never enter
the table-generation path.

| Pattern | Point progression Keys/s | Fixed-base window Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 5,558,876.5 | 18,642,975.0 | 3.35x | 97.0% | 56.0 C | 92.21 W | 1,715 MiB |
| Literal Suffix | 5,829,812.0 | 18,210,105.0 | 3.12x | 97.1% | 59.2 C | 93.63 W | 1,704 MiB |
| Repeat Prefix | 5,873,825.0 | 18,049,290.0 | 3.07x | 97.1% | 61.9 C | 95.04 W | 1,694 MiB |
| Repeat Suffix | 5,627,305.5 | 18,455,700.0 | 3.28x | 97.0% | 59.2 C | 94.91 W | 1,690 MiB |
| **Mean** | **5,722,454.8** | **18,339,517.5** | **3.20x** | **97.1%** | **59.1 C** | **93.95 W** | **1,700.8 MiB** |

This is 24.94x faster than the same-session 735,263.9 Keys/s scheduling
baseline. Static usage remains 244 registers per thread, while stack usage
drops slightly to 1,472 bytes.

## Field-inverse addition-chain checkpoint

The batch inverse now uses a fixed addition chain for the exact exponent
`p - 2 = 2^256 - 2^32 - 979`. It requires 255 field squarings and 15 general
field multiplies, versus 255 squarings and 249 multiplies in the binary loop.

| Pattern | Fixed-base window Keys/s | Addition chain Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 18,642,975.0 | 19,632,740.0 | 1.05x | 96.6% | 53.9 C | 89.72 W | 1,681 MiB |
| Literal Suffix | 18,210,105.0 | 20,003,820.0 | 1.10x | 97.0% | 57.6 C | 92.14 W | 1,683 MiB |
| Repeat Prefix | 18,049,290.0 | 20,887,305.0 | 1.16x | 96.8% | 60.6 C | 93.74 W | 1,681 MiB |
| Repeat Suffix | 18,455,700.0 | 19,993,035.0 | 1.08x | 97.0% | 61.4 C | 94.67 W | 1,681 MiB |
| **Mean** | **18,339,517.5** | **20,129,225.0** | **1.10x** | **96.9%** | **58.4 C** | **92.57 W** | **1,681.5 MiB** |

The cumulative multiplier over the same-session scheduling baseline is
27.38x. Static kernel usage remains 244 registers per thread and 1,472 bytes
of stack.

## Chunked Base58 checkpoint

The exact Base58Check encoder now treats the 25-byte checked payload as seven
big-endian 32-bit limbs and divides by `58^5`, emitting five Base58 digits per
pass. This replaces the correctness-first byte-by-byte conversion without
changing or approximating any address digit; double SHA-256 still runs for
every key.

| Pattern | Addition chain Keys/s | Chunked Base58 Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 19,632,740.0 | 26,532,755.0 | 1.35x | 97.2% | 53.4 C | 85.52 W | 1,541 MiB |
| Literal Suffix | 20,003,820.0 | 25,016,255.0 | 1.25x | 97.2% | 56.5 C | 86.42 W | 1,541 MiB |
| Repeat Prefix | 20,887,305.0 | 25,865,020.0 | 1.24x | 97.5% | 59.2 C | 87.78 W | 1,541 MiB |
| Repeat Suffix | 19,993,035.0 | 26,038,155.0 | 1.30x | 97.3% | 61.6 C | 88.68 W | 1,541 MiB |
| **Mean** | **20,129,225.0** | **25,863,046.3** | **1.28x** | **97.3%** | **57.7 C** | **87.10 W** | **1,541 MiB** |

The cumulative multiplier over the same-session scheduling baseline is
35.17x. Static kernel usage is 244 registers per thread and 1,440 bytes of
stack.

## Batch-inversion width checkpoint

Increasing the contiguous per-thread group from 8 to 16 keys amortizes each
fixed-base start and field inverse across twice as many candidates. The search
batch increases proportionally from 65,536 to 131,072 keys, preserving 8,192
independent groups and short control boundaries.

| Pattern | Width 8 Keys/s | Width 16 Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 26,532,755.0 | 43,175,820.0 | 1.63x | 96.9% | 52.4 C | 88.25 W | 1,617 MiB |
| Literal Suffix | 25,016,255.0 | 45,138,225.0 | 1.80x | 97.2% | 55.9 C | 90.22 W | 1,617 MiB |
| Repeat Prefix | 25,865,020.0 | 43,712,210.0 | 1.69x | 97.8% | 58.9 C | 91.27 W | 1,617 MiB |
| Repeat Suffix | 26,038,155.0 | 43,499,040.0 | 1.67x | 97.8% | 61.5 C | 92.40 W | 1,617 MiB |
| **Mean** | **25,863,046.3** | **43,881,323.8** | **1.70x** | **97.4%** | **57.2 C** | **90.54 W** | **1,617 MiB** |

The cumulative multiplier over the same-session scheduling baseline is
59.68x. Static kernel usage remains 244 registers per thread; the wider local
point/prefix arrays increase stack use to 2,496 bytes per thread.

Increasing the same group to 32 keys produced another clear net gain:

| Pattern | Width 16 Keys/s | Width 32 Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 43,175,820.0 | 66,345,760.0 | 1.54x | 98.0% | 59.0 C | 95.33 W | 1,775 MiB |
| Literal Suffix | 45,138,225.0 | 66,645,270.0 | 1.48x | 98.0% | 61.8 C | 96.99 W | 1,775 MiB |
| Repeat Prefix | 43,712,210.0 | 65,460,570.0 | 1.50x | 98.0% | 59.4 C | 96.37 W | 1,775 MiB |
| Repeat Suffix | 43,499,040.0 | 65,411,660.0 | 1.50x | 98.0% | 56.5 C | 95.13 W | 1,775 MiB |
| **Mean** | **43,881,323.8** | **65,965,815.0** | **1.50x** | **98.0%** | **59.2 C** | **95.96 W** | **1,775 MiB** |

The cumulative multiplier over the same-session scheduling baseline is
89.72x. The 32-key group uses 244 registers and 4,608 bytes of stack per
thread; the corresponding search batch is 262,144 keys.

The 64-key group remains a net win despite its larger local-memory footprint:

| Pattern | Width 32 Keys/s | Width 64 Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 66,345,760.0 | 89,262,465.0 | 1.35x | 98.8% | 53.6 C | 98.41 W | 2,087 MiB |
| Literal Suffix | 66,645,270.0 | 90,154,755.0 | 1.35x | 98.9% | 57.6 C | 100.02 W | 2,087 MiB |
| Repeat Prefix | 65,460,570.0 | 88,756,410.0 | 1.36x | 99.0% | 60.8 C | 101.11 W | 2,087 MiB |
| Repeat Suffix | 65,411,660.0 | 88,689,400.0 | 1.36x | 98.8% | 61.0 C | 102.17 W | 2,087 MiB |
| **Mean** | **65,965,815.0** | **89,215,757.5** | **1.35x** | **98.9%** | **58.3 C** | **100.43 W** | **2,087 MiB** |

The cumulative multiplier over the same-session scheduling baseline is
121.35x. The 64-key group uses 244 registers and 8,832 bytes of stack per
thread; the corresponding search batch is 524,288 keys.

The final tuning point uses 128 keys per group and crosses 100 MKeys/s under
the same exact pipeline:

| Pattern | Width 64 Keys/s | Width 128 Keys/s | Multiplier | GPU util. | Temperature | Power | Peak GPU VRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| Literal Prefix | 89,262,465.0 | 108,987,150.0 | 1.22x | 99.0% | 54.2 C | 102.39 W | 2,715 MiB |
| Literal Suffix | 90,154,755.0 | 108,868,200.0 | 1.21x | 99.0% | 58.4 C | 104.45 W | 2,715 MiB |
| Repeat Prefix | 88,756,410.0 | 108,550,400.0 | 1.22x | 99.0% | 61.8 C | 106.38 W | 2,715 MiB |
| Repeat Suffix | 88,689,400.0 | 108,155,900.0 | 1.22x | 99.0% | 60.1 C | 106.31 W | 2,715 MiB |
| **Mean** | **89,215,757.5** | **108,640,412.5** | **1.22x** | **99.0%** | **58.6 C** | **104.88 W** | **2,715 MiB** |

The final cumulative multiplier over the same-session 735,263.9 Keys/s
scheduling baseline is 147.76x. Static kernel usage is 244 registers and
17,280 bytes of stack per thread; the corresponding search batch is 1,048,576
keys. This large local-memory footprint is the principal remaining
architecture-level risk and should be revisited with a split-kernel or
block-level scan design rather than increasing the group further.

## Final stability and product validation

The width-128 checkpoint completed a 600-second GPU-only validation with a
10-second synchronized pause at the midpoint:

- 108,903,194.9 average GPU and total Keys/s
- 99.0% average GPU utilization
- 63 C maximum temperature and 107.89 W maximum power
- 2,725 MiB maximum system-wide GPU memory used
- 1,504.4 to 1,504.5 MiB process private memory
- monotonic checked count, Pause/Resume PASS, Stop PASS, zero API failures,
  and no CUDA/pipeline error

A separate 300-second CPU+GPU validation, also with a 10-second midpoint
pause, averaged 106,158,534.5 GPU Keys/s plus 477,616.0 CPU Keys/s for
106,636,137.9 total Keys/s. It also passed monotonic-count,
Pause/Resume/Stop, and zero-error checks; process private memory remained
between 1,507.4 and 1,507.5 MiB.

The real Dashboard/API smoke test loaded the embedded HTML, reported RTX 5070
enabled and CPU disabled by default, showed identical GPU and Total Hashrate
at 109,034,000 Keys/s, advanced the checked count, and stopped cleanly. The
final executable test suite passed 33/33 tests, including fixed CUDA vectors,
all Literal/Repeat Prefix/Suffix/Both modes, curve-order bounds, heterogeneous
range disjointness, candidate CPU verification, lifecycle controls, NVML, and
Web API serialization.

## Scheduling-checkpoint profile findings

Nsight Compute 2026.2.1 discovered the `vanity_search_kernel`, but Windows
denied access to hardware performance counters with `ERR_NVGPUCTRPERM`.
Consequently this run does not claim measured warp-stall or achieved-occupancy
figures. CUDA static resource inspection with `cuobjdump --dump-resource-usage`
reported 148 registers per thread and 336 bytes of stack.

The checkpoint launched only eight candidates per batch: one partially filled
warp and one block for the entire GPU, followed by host synchronization and
counter copies. That launch geometry and synchronization dominated throughput.
The optimized batch contains 8,192 independent candidates, which fills all SMs
while retaining short Pause/Stop boundaries on WDDM.

Within each candidate, secp256k1 field and point arithmetic remains the largest
algorithmic cost: scalar multiplication performs hundreds of point operations,
then Fermat inversion performs a fixed chain of field squares and multiplies.
Keccak-256, SHA-256/Base58Check, and pattern matching occur only once after the
public point is available. Random scalar bits cause some point-add divergence;
the final pattern branch is inexpensive and happens after the expensive common
pipeline. The 148-register footprint remains the main occupancy risk.

Point progression, fixed-base windows, and batch inversion could provide a
further large gain, but they change the mathematical execution structure. They
were deliberately deferred rather than mixed into this low-risk scheduling
checkpoint.

## Scheduling-checkpoint multi-device results

Each configuration ran the real backend for 20 seconds after a 5-second warmup.

| Configuration | CPU Keys/s | GPU Keys/s | Total Keys/s |
|---|---:|---:|---:|
| CPU only | 457,644.1 | 0 | 457,644.1 |
| RTX 5070 only | 0 | 726,602.1 | 726,602.1 |
| RTX 5070 + CPU | 463,016.3 | 741,846.7 | 1,204,862.7 |

CPU and CUDA workers continue to acquire disjoint ranges from the shared atomic
search-space allocator. The full correctness suite includes an explicit
heterogeneous-range overlap test.

## Scheduling-checkpoint product validation

- 32/32 CPU, CUDA, NVML, search-space, lifecycle, and Web API tests passed.
- A GPU-only Literal Suffix `1` search completed in one 8,192-key batch.
- The candidate was independently CPU-verified, saved locally, and exposed to
  the Dashboard as public address
  `TY59abQgLQjfhp6r8UQKMfY74DFhtkSo21` from device `cuda-0`/`CUDA`.
- A separate Python implementation using `cryptography` secp256k1 plus an
  independent Keccak/Base58Check implementation re-derived exactly the saved
  public address from the saved private key. The private key was not printed.
- Edge/Playwright verified real CPU and RTX 5070 device cards, all NVML fields,
  the live hashrate chart, Start/Pause/Resume/Stop, Dashboard refresh, and
  continued engine progress after the browser closed. Desktop and 390x844
  mobile viewports had no console errors; the mobile viewport had zero
  horizontal overflow after the responsive fix.

## Scheduling-checkpoint stability

The final build ran CPU and RTX 5070 together for 1,868.4 wall-clock seconds.
After excluding the synchronized 10-second pause, the engine reported 1,853.3
seconds (30 minutes 53.3 seconds) of active search and 2,281,569,299 checked
keys, equivalent to 1,231,084.7 aggregate Keys/s over the active interval.

- Checked count was monotonic; API failures and CUDA errors were both zero.
- Pause held the checked count exactly constant for 10 seconds; Resume restored
  progress, and Stop ended in `STOPPED`.
- Process private memory stayed at approximately 289.1 MiB throughout the
  sampled run.
- System-wide GPU memory fluctuated with other desktop clients rather than
  growing monotonically. It was 1,687 MiB before search, ranged from 1,545 to
  1,858 MiB in the minute observations, and was 1,530 MiB five seconds after
  Stop.
- Observed minute samples remained at or below 62 C and 85.87 W.
- Dashboard refresh preserved `RUNNING` state and increasing counts. Closing
  the browser did not stop the engine.
