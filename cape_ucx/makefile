O_FOLDER    = obj
L_FOLDER    = lib
C_FOLDER    = src
I_FOLDER    = include
EXE_FOLDER  = bin

# ── UCX ───────────────────────────────────────────────────────────────────────
# Override on the command line, e.g.:
#   make cape_mamult UCX_SRC=/opt/ucx/include UCX_LIB=/opt/ucx/lib
UCX_SRC     ?= /usr/local/src/ucx/src
UCX_GEN     ?= /home/yassir/internship/code/matmul_bench/ucx_gen
UCX_LIB     ?= /home/yassir/internship/code/matmul_bench/ucx_libs

# ── UCC (used by the bitmap master/slave monitor for broadcast) ──────────────
# Override on the command line, e.g.:
#   make dickpt_bitmap_master_slave_monitor UCC_SRC=/opt/ucc/include UCC_LIB=/opt/ucc/lib
UCC_SRC     ?=
UCC_LIB     ?=
comma       := ,
UCC_FLAGS   ?= $(if $(UCC_SRC),-I$(UCC_SRC),$(shell pkg-config --cflags ucc 2>/dev/null))
UCC_LINK    ?= $(if $(UCC_LIB),-L$(UCC_LIB) -lucc -Wl$(comma)-rpath$(comma)$(UCC_LIB),$(shell pkg-config --libs ucc 2>/dev/null))

# ── PMIx (optional) ───────────────────────────────────────────────────────────
# Pass PMIX_FLAGS and PMIX_LINK from the build script when PMIx is available.
# Leave empty to build without PMIx (uses SLURM env vars + shared filesystem).
PMIX_FLAGS  ?=
PMIX_LINK   ?=

# ── Profiling (build with PROFILE=1 to enable, e.g. make cape_mamult PROFILE=1)
PROFILE ?= 1
ifeq ($(PROFILE),1)
PROF_FLAGS = -DCAPE_PROFILE
else
PROF_FLAGS =
endif

# ── Compiler & flags ─────────────────────────────────────────────────────────
CC      ?= cc
CFLAGS   = -O2 -I./$(I_FOLDER) -I$(UCX_SRC) -I$(UCX_GEN) $(PMIX_FLAGS) $(PROF_FLAGS)
LDFLAGS  = -L$(UCX_LIB) -lucp -lucs $(PMIX_LINK) -Wl,-rpath,$(UCX_LIB)
OMP_CFLAGS = -O2 -fopenmp

# ══════════════════════════════════════════════════════════════════════════════
# TIMESTAMP version (cape.c)
#   cape.c is a library compiled together with the application into one binary.
#   make cape_mamult
# ══════════════════════════════════════════════════════════════════════════════
COMPILE  = $(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape.c
# Bitmap variant: same interface, different on-the-wire delta format
# ([page_bmp][word_bmp][full_page] instead of sparse [addr][len][data]).
COMPILE_BITMAP = $(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape_bitmap.c

# ── Default target ────────────────────────────────────────────────────────────
all: cape_mamult cape_reduction cape_nested_parallel_sections cape_memwrite

# ── Per-application targets (timestamp) ──────────────────────────────────────
cape_mamult:
	$(COMPILE) $(C_FOLDER)/apps/cape_mamult.c -o $(EXE_FOLDER)/cape_mamult $(LDFLAGS)

cape_matvec:
	$(COMPILE) $(C_FOLDER)/apps/cape_matvec.c -o $(EXE_FOLDER)/cape_matvec $(LDFLAGS)

cape_gradient:
	$(COMPILE) $(C_FOLDER)/apps/cape_gradient.c -o $(EXE_FOLDER)/cape_gradient $(LDFLAGS)

cape_memwrite:
	$(COMPILE) $(C_FOLDER)/apps/cape_memwrite.c -o $(EXE_FOLDER)/cape_memwrite $(LDFLAGS)

cape_write_stress:
	$(COMPILE) $(C_FOLDER)/apps/cape_write_stress.c -o $(EXE_FOLDER)/cape_write_stress $(LDFLAGS)

cape_bitmap_write_stress:
	$(COMPILE_BITMAP) $(C_FOLDER)/apps/cape_write_stress.c -o $(EXE_FOLDER)/cape_bitmap_write_stress $(LDFLAGS)

cape_mamult2d:
	$(COMPILE) $(C_FOLDER)/apps/cape_mamult2d.c -o $(EXE_FOLDER)/cape_mamult2d $(LDFLAGS)

cape_pi:
	$(COMPILE) $(C_FOLDER)/apps/cape_pi.c -o $(EXE_FOLDER)/cape_pi $(LDFLAGS)

cape_prime:
	$(COMPILE) $(C_FOLDER)/apps/cape_prime.c -o $(EXE_FOLDER)/cape_prime $(LDFLAGS)

cape_prime_number:
	$(COMPILE) $(C_FOLDER)/apps/cape_prime_number.c -o $(EXE_FOLDER)/cape_prime_number $(LDFLAGS)

cape_reduction:
	$(COMPILE) $(C_FOLDER)/apps/cape_reduction.c -o $(EXE_FOLDER)/cape_reduction $(LDFLAGS)

cape_for:
	$(COMPILE) $(C_FOLDER)/apps/cape_for.c -o $(EXE_FOLDER)/cape_for $(LDFLAGS)

cape_for_nowait:
	$(COMPILE) $(C_FOLDER)/apps/cape_for_nowait.c -o $(EXE_FOLDER)/cape_for_nowait $(LDFLAGS)

cape_parallel_for:
	$(COMPILE) $(C_FOLDER)/apps/cape_parallel_for.c -o $(EXE_FOLDER)/cape_parallel_for $(LDFLAGS)

cape_vector1:
	$(COMPILE) $(C_FOLDER)/apps/cape_vector1.c -o $(EXE_FOLDER)/cape_vector1 $(LDFLAGS)

cape_vector2:
	$(COMPILE) $(C_FOLDER)/apps/cape_vector2.c -o $(EXE_FOLDER)/cape_vector2 $(LDFLAGS)

cape_critical:
	$(COMPILE) $(C_FOLDER)/apps/cape_critical.c -o $(EXE_FOLDER)/cape_critical $(LDFLAGS)

cape_critical2:
	$(COMPILE) $(C_FOLDER)/apps/cape_critical2.c -o $(EXE_FOLDER)/cape_critical2 $(LDFLAGS)

cape_sections_nowait:
	$(COMPILE) $(C_FOLDER)/apps/cape_sections_nowait.c -o $(EXE_FOLDER)/cape_sections_nowait $(LDFLAGS)

cape_nested_parallel_sections:
	$(COMPILE) $(C_FOLDER)/apps/cape_nested_parallel_sections.c -o $(EXE_FOLDER)/cape_nested_parallel_sections $(LDFLAGS)

cape_test_heap:
	$(COMPILE) $(C_FOLDER)/apps/cape_test_heap.c -o $(EXE_FOLDER)/cape_test_heap $(LDFLAGS)

omp_gradient:
	$(CC) -w $(OMP_CFLAGS) $(C_FOLDER)/omp_apps/omp_gradient.c -o $(EXE_FOLDER)/omp_gradient

apps: cape_mamult cape_mamult2d cape_matvec cape_gradient cape_memwrite \
      cape_pi cape_prime cape_prime_number \
      cape_reduction cape_for cape_for_nowait cape_parallel_for \
      cape_vector1 cape_vector2 cape_critical cape_critical2 \
      cape_sections_nowait cape_nested_parallel_sections cape_test_heap
 
# ══════════════════════════════════════════════════════════════════════════════
# DICKPT version (cape_incr.c + dickpt apps)
#   The monitor (cape_incr.c) is a standalone ptrace-based process.
#   Apps use int3 breakpoints via cape_dickpt.h to signal the monitor.
#   Two-step build:
#     1. make dickpt_monitor     — builds the monitor binary
#     2. make dickpt_mul_manual  — builds a dickpt app (plain binary, no UCX)
#   Usage: srun bin/cape_dickpt_monitor bin/dickpt_mul_manual
# ══════════════════════════════════════════════════════════════════════════════

# Dickpt apps are plain binaries — no UCX, no cape.c library.
# They only need the include dir for cape_dickpt.h / cape_signal.h.
#
# -no-pie / -fno-pie: build as non-PIE so the executable's text/data/bss
# get linker-assigned fixed VAs. Combined with personality(ADDR_NO_RANDOMIZE)
# in cape_dickpt_runtime.c, every rank sees identical VAs for globals —
# required because DICKPT checkpoints carry absolute addresses that must be
# injectable into any rank's address space.
# Workaround for /usr/bin/ld being too old to decompress zstd-compressed
# .debug_info in libgcc.a (kills the link at -O2 once helpers like __muldi3
# are pulled in). We discover a newer binutils via the module system at
# make-time and point gcc at its `ld` with -B<dir>/.
# Only run binutils discovery (and print the [dickpt] info lines) when an
# actual dickpt target is being built. Cape targets don't need this.
DICKPT_TARGETS := dickpt dickpt_monitor dickpt_bitmap_monitor dickpt_bitmap_master_slave_monitor dickpt_mul_manual dickpt_matvec_manual dickpt_gradient_manual dickpt_memwrite_manual dickpt_write_stress
ifneq ($(filter $(DICKPT_TARGETS),$(MAKECMDGOALS)),)
ifneq ($(EBROOTBINUTILS),)
DICKPT_BINUTILS_BIN := $(EBROOTBINUTILS)/bin
DICKPT_BINUTILS_SRC := EBROOTBINUTILS env
else
DICKPT_BINUTILS_BIN := $(shell bash -c '\
    for f in /etc/profile.d/lmod.sh /etc/profile.d/z00_lmod.sh /etc/profile.d/modules.sh /usr/share/lmod/lmod/init/bash; do \
        [ -r $$f ] && . $$f; \
    done; \
    module load binutils/2.42-GCCcore-14.2.0 >/dev/null 2>&1 && echo $$EBROOTBINUTILS/bin')
DICKPT_BINUTILS_SRC := module-load fallback
endif
ifeq ($(DICKPT_BINUTILS_BIN),)
$(warning [dickpt] could NOT locate a newer binutils — link will likely fail with /usr/bin/ld decompress error)
$(warning [dickpt]   EBROOTBINUTILS='$(EBROOTBINUTILS)'  PATH='$(PATH)')
DICKPT_LD_PREFIX :=
else
$(info [dickpt] binutils bin: $(DICKPT_BINUTILS_BIN)  (source: $(DICKPT_BINUTILS_SRC)))
DICKPT_LD_PREFIX := -B$(DICKPT_BINUTILS_BIN)/
endif
endif
DICKPT_COMPILE = $(CC) -w -O2 -mno-red-zone -I./$(I_FOLDER) -no-pie -fno-pie $(DICKPT_LD_PREFIX)
DICKPT_RUNTIME = $(C_FOLDER)/apps/cape_dickpt_runtime.c

# Monitor (needs UCX for inter-node communication)
dickpt_monitor:
	@mkdir -p $(EXE_FOLDER)
	$(CC) -w $(CFLAGS) $(DICKPT_LD_PREFIX) $(C_FOLDER)/monitor/cape_incr.c -o $(EXE_FOLDER)/cape_dickpt_monitor $(LDFLAGS)

dickpt_bitmap_monitor:
	@mkdir -p $(EXE_FOLDER)
	$(CC) -w $(CFLAGS) $(DICKPT_LD_PREFIX) $(C_FOLDER)/monitor/cape_incr_bitmap.c -o $(EXE_FOLDER)/cape_dickpt_bitmap_monitor $(LDFLAGS)

dickpt_bitmap_master_slave_monitor:
	@mkdir -p $(EXE_FOLDER)
	$(CC) -w $(CFLAGS) $(UCC_FLAGS) -DUSE_UCC_BCAST $(DICKPT_LD_PREFIX) $(C_FOLDER)/monitor/cape_incr_bitmap_master_slave.c -o $(EXE_FOLDER)/cape_dickpt_bitmap_master_slave_monitor $(LDFLAGS) $(UCC_LINK)

# Dickpt application targets
dickpt_mul_manual:
	@mkdir -p $(EXE_FOLDER)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_mul_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_mul_manual

dickpt_matvec_manual:
	@mkdir -p $(EXE_FOLDER)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_matvec_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_matvec_manual

dickpt_gradient_manual:
	@mkdir -p $(EXE_FOLDER)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_gradient_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_gradient_manual

dickpt_memwrite_manual:
	@mkdir -p $(EXE_FOLDER)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_memwrite_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_memwrite_manual

dickpt_write_stress:
	@mkdir -p $(EXE_FOLDER)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_write_stress_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_write_stress

dickpt_reduction_manual:
	@mkdir -p $(EXE_FOLDER)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_reduction_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_reduction_manual

# Build monitor + all dickpt apps
dickpt: dickpt_monitor dickpt_mul_manual dickpt_matvec_manual dickpt_gradient_manual dickpt_memwrite_manual dickpt_write_stress dickpt_reduction_manual

# ── Clean ─────────────────────────────────────────────────────────────────────
cleanall:
	rm -f $(EXE_FOLDER)/* $(O_FOLDER)/* $(L_FOLDER)/*

.PHONY: all apps cleanall \
        dickpt dickpt_monitor dickpt_bitmap_monitor dickpt_bitmap_master_slave_monitor dickpt_mul_manual \
        dickpt_matvec_manual dickpt_gradient_manual dickpt_memwrite_manual \
        dickpt_write_stress dickpt_reduction_manual \
        cape_mamult cape_mamult2d cape_matvec cape_gradient cape_memwrite \
        cape_write_stress cape_bitmap_write_stress \
        cape_pi cape_prime cape_prime_number \
        cape_reduction cape_for cape_for_nowait cape_parallel_for \
        cape_vector1 cape_vector2 cape_critical cape_critical2 omp_gradient \
        cape_sections_nowait cape_nested_parallel_sections cape_test_heap
