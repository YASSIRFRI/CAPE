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

# ── Default target ────────────────────────────────────────────────────────────
all: cape_mamult cape_reduction cape_nested_parallel_sections 

# ── Per-application targets (timestamp) ──────────────────────────────────────
cape_mamult:
	$(COMPILE) $(C_FOLDER)/apps/cape_mamult.c -o $(EXE_FOLDER)/cape_mamult $(LDFLAGS)

cape_matvec:
	$(COMPILE) $(C_FOLDER)/apps/cape_matvec.c -o $(EXE_FOLDER)/cape_matvec $(LDFLAGS)

cape_gradient:
	$(COMPILE) $(C_FOLDER)/apps/cape_gradient.c -o $(EXE_FOLDER)/cape_gradient $(LDFLAGS)

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

apps: cape_mamult cape_mamult2d cape_matvec cape_gradient \
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
# Workaround for binutils that can't decompress zstd-compressed .debug_info
# sections in the system libgcc.a (which kills the link at -O2 once libgcc
# helpers like __muldi3 are pulled in). We copy libgcc.a to a private dir,
# strip its debug sections, and point gcc at it via -B<dir>/.
DICKPT_LIBGCC_DIR := /tmp/$(shell id -un)/cape_dickpt_libgcc
DICKPT_LIBGCC     := $(DICKPT_LIBGCC_DIR)/libgcc.a

$(DICKPT_LIBGCC):
	@mkdir -p $(DICKPT_LIBGCC_DIR)
	@cp $$($(CC) -print-libgcc-file-name) $@
	@strip --strip-debug $@ 2>/dev/null || objcopy --strip-debug $@

DICKPT_COMPILE = $(CC) -w -O2 -mno-red-zone -B$(DICKPT_LIBGCC_DIR)/ -I./$(I_FOLDER) -no-pie -fno-pie
DICKPT_RUNTIME = $(C_FOLDER)/apps/cape_dickpt_runtime.c

# Monitor (needs UCX for inter-node communication)
dickpt_monitor:
	$(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape_incr.c -o $(EXE_FOLDER)/cape_dickpt_monitor $(LDFLAGS)

# Dickpt application targets
dickpt_mul_manual: $(DICKPT_LIBGCC)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_mul_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_mul_manual

dickpt_matvec_manual: $(DICKPT_LIBGCC)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_matvec_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_matvec_manual

dickpt_gradient_manual: $(DICKPT_LIBGCC)
	$(DICKPT_COMPILE) $(C_FOLDER)/apps/cape_gradient_manual.c $(DICKPT_RUNTIME) -o $(EXE_FOLDER)/dickpt_gradient_manual

# Build monitor + all dickpt apps
dickpt: dickpt_monitor dickpt_mul_manual dickpt_matvec_manual dickpt_gradient_manual

# ── Clean ─────────────────────────────────────────────────────────────────────
cleanall:
	rm -f $(EXE_FOLDER)/* $(O_FOLDER)/* $(L_FOLDER)/*

.PHONY: all apps cleanall \
        dickpt dickpt_monitor dickpt_mul_manual \
        dickpt_matvec_manual dickpt_gradient_manual \
        cape_mamult cape_mamult2d cape_matvec cape_gradient \
        cape_pi cape_prime cape_prime_number \
        cape_reduction cape_for cape_for_nowait cape_parallel_for \
        cape_vector1 cape_vector2 cape_critical cape_critical2 omp_gradient \
        cape_sections_nowait cape_nested_parallel_sections cape_test_heap
