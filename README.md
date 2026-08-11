# CAPE

CAPE transforms OpenMP-style C programs into distributed CAPE or DICKPT programs and provides the runtime pieces needed to build and run them on a cluster.

This repository ships the TXL binary battery-included at `transform/dompcc/txl`, so a separate TXL install is not required unless you want to override it with `TXL_BIN=/path/to/txl`.

## Repository layout

- `bin/`: built monitors and applications
- `include/`: public headers
- `src/monitor/`: CAPE and DICKPT runtime/monitor sources
- `src/apps/`: sample and generated applications
- `scripts/deploy/`: cluster provisioning and copy scripts
- `scripts/bench/`: SLURM launch, benchmark, and verification scripts
- `transform/`: TXL-based OpenMP to CAPE/DICKPT transformers, including the bundled `txl` binary
- `docs/`: design notes, benchmark data, and figures
- `archive/legacy/`: retained historical code

## Install

1. Install the native build/runtime dependencies on your Linux cluster nodes:
   - a C compiler (`gcc` or compatible)
   - UCX
   - optionally PMIx
   - optionally UCC for the bitmap master/slave monitor variants
2. Clone or unpack CAPE on the build node.
3. Configure the cluster addresses in `scripts/deploy/ip_config.sh`.
4. Build the binaries you want from the repo root. For example:

```bash
make cape_mamult
make dickpt_bitmap_monitor dickpt_task_manual
```

5. Deploy the package to the worker nodes:

```bash
./scripts/deploy/deploy_cape.sh
```

`deploy_cape.sh` now copies `bin/`, `include/`, `src/`, `scripts/`, `transform/`, `makefile`, and `README.md`, so the installed copy includes both the bundled TXL toolchain and the source tree needed to rebuild or regenerate apps.

## Demo

### 1. Transform an OpenMP example with the bundled TXL binary

Build a CAPE version of the sample task test directly from the shipped TXL workspace:

```bash
cd transform/txl/openmptocape
./build.sh verify_task.c verify_task
```

`build.sh` prefers the bundled `transform/dompcc/txl` binary automatically and produces:

- `verify_task_cape.c`: the generated CAPE source
- `verify_task`: the compiled CAPE executable

### 2. Build and run an existing CAPE example

From the repo root:

```bash
make cape_mamult
```

Then launch it with the appropriate cluster script for your environment, for example:

```bash
sbatch scripts/bench/cape_benchmark.sh
```

### 3. Regenerate a DICKPT task app from OpenMP source

The DICKPT verification scripts also use the bundled TXL binary by default. For example:

```bash
REGEN=1 sbatch scripts/bench/dickpt_bitmap_task_manual_test.sh
```

That regenerates `src/apps/cape_task_manual.c` from `transform/txl/openmptodickpt/tests/test_task.c`, builds the bitmap monitor plus app, and runs the distributed test.

## Developer notes

- CAPE in-process runtime: `src/monitor/cape.c` and `src/monitor/cape_bitmap.c`
- DICKPT monitor runtime: `src/monitor/cape_incr*.c`
- Application sources: `src/apps/*.c`
- To add a new hand-written app, place it in `src/apps/` and add a target in `makefile`

