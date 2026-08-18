# PAPI memory profiling + CAPE vs OpenMP on heat3d

## 1. CAPE (bitmap multithreaded monitor) vs plain OpenMP

Both run the same 7-point Jacobi kernel on the same `N x N x N` cube with the
same conditional write-back, and print the same `RESULT` key=value line.

| | source | parallelism | checkpointing |
|---|---|---|---|
| CAPE | `src/apps/cape_heat3d_manual.c` + `src/monitor/cape_incr_bitmap_multithreaded.c` | 1 rank/node × clone(2) threads | DICKPT per iteration |
| OpenMP | `src/omp_apps/omp_heat3d.c` | 1 node × OpenMP threads | none |

Run:

```bash
sbatch scripts/bench/bench_job_heat3d_dickpt.sh          # cape, thread sweep
sbatch scripts/bench/bench_job_heat3d_omp.sh             # omp,  thread sweep
scripts/bench/compare_heat3d_cape_vs_omp.sh \
    results/bench_heat3d_dickpt_<jobid>/bench_heat3d_dickpt_<jobid>.csv \
    results/bench_heat3d_omp_<jobid>/bench_heat3d_omp_<jobid>.csv
```

Knobs (both scripts): `N_DIM`, `N_ITERS`, `THREADS_LIST`, `REPS`, `PAPI`.

The comparison table reports:

- `compute_ratio` = `cape(sweep+wb) × nodes / omp(sweep+wb)`. ~1.0 means the
  clone(2) compute threads match OpenMP; >1 means the tracked region itself is
  slower (write-protect faults, cache effects).
- `ckpt_%` = share of CAPE runtime inside the DICKPT path — the overhead to
  attack.
- `e2e_speedup` = `omp_ms / cape_ms` — the end-to-end win from distribution.

OpenMP runs the whole cube on one node while each CAPE rank owns `1/nodes` of
it (but replicates full state each iteration), hence the `× nodes` scaling in
`compute_ratio`.

## 2. PAPI hardware counters

`include/cape_papi.h` is a header-only, zero-cost-when-off wrapper. Build with
`PAPI=1` (`PAPI_ROOT=` for a non-pkg-config install):

```bash
make dickpt_bitmap_multithreaded_monitor dickpt_heat3d_manual PAPI=1
make omp_heat3d PAPI=1
# or, via the bench jobs:
PAPI=1 sbatch scripts/bench/bench_job_heat3d_dickpt.sh
PAPI=1 sbatch scripts/bench/bench_job_heat3d_omp.sh
```

Instrumented regions:

| region | where | what it measures |
|---|---|---|
| `generate_ckpt` | monitor, `generate_checkpoint()` | whole checkpoint generation on the monitor thread |
| `par_for_lane` | monitor, every `cape_par_for` lane | the parallel dirty-set walk: `process_vm_readv` + word diff — **the suspected bottleneck** |
| `app_sweep`, `app_writeback` | CAPE app, main-thread slab | application compute, for scale |
| `omp_sweep`, `omp_writeback` | OpenMP app, all threads | baseline compute |

Counters are per-thread EventSets accumulated atomically into the region, so a
region touched by N threads reports the sum over threads. Regions may nest
(the probe handle lives on the caller's stack).

Default events (`CAPE_PAPI_EVENTS` overrides, comma-separated; unsupported
events are dropped with a warning rather than failing the run):

```
PAPI_TOT_CYC,PAPI_TOT_INS,PAPI_LD_INS,PAPI_SR_INS,
PAPI_L1_DCM,PAPI_L2_DCM,PAPI_L3_TCM,PAPI_TLB_DM
```

Derived lines printed per region: `IPC`, `L1_miss/mem_ref`, and `est_dram_bw`
(`PAPI_L3_TCM × 64 B / elapsed`).

### Reading the result

The hypothesis "CAPE is memory-bound in the checkpoint path" is confirmed if,
in `par_for_lane` relative to `omp_sweep`:

- IPC is much lower (stalled on memory, not issuing work),
- `L1_miss/mem_ref` is high (streaming, no reuse — expected for a diff over
  pages read out of another process),
- `est_dram_bw` approaches the node's STREAM bandwidth (saturated), and
- `PAPI_TLB_DM` is large (page-granular access over a big region).

If instead `par_for_lane` shows high IPC and low miss rates while `gen_ms`
stays large, the cost is in syscalls (`process_vm_readv` / `UFFDIO_WRITEPROTECT`
ioctls) — compare against the `process_vm_read_ns` / `writeprotect_ns` counters
in the `[DICKPT PROFILE]` dump (`PROFILE=1`, on by default).

### Caveats

- In `cape_heat3d_manual.c` only slab 0 (the main thread) is measured: the
  workers are raw `clone(2)` threads without `CLONE_SETTLS`, so they share the
  parent's TLS and cannot hold per-thread EventSets. Slabs are equal-sized —
  multiply by the thread count for a node figure.
- PAPI needs `perf_event_paranoid <= 2` (usually fine) and enough PMU slots; if
  the event set fails to start, trim `CAPE_PAPI_EVENTS`.
