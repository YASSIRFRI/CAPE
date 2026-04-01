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
PROFILE ?= 0
ifeq ($(PROFILE),1)
PROF_FLAGS = -DCAPE_PROFILE
else
PROF_FLAGS =
endif

# ── Compiler & flags ─────────────────────────────────────────────────────────
CC      ?= cc
CFLAGS   = -I./$(I_FOLDER) -I$(UCX_SRC) -I$(UCX_GEN) $(PMIX_FLAGS) $(PROF_FLAGS)
LDFLAGS  = -L$(UCX_LIB) -lucp -lucs $(PMIX_LINK) -Wl,-rpath,$(UCX_LIB)

# ── Timestamp version (cape.c) ───────────────────────────────────────────────
# cape.c is a library compiled together with the application.
#   make cape_mamult          — builds bin/cape_mamult
COMPILE  = $(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape.c

# ── Default target ────────────────────────────────────────────────────────────
all: cape_mamult cape_reduction

# ── Per-application targets (timestamp version) ─────────────────────────────
cape_mamult:
	$(COMPILE) $(C_FOLDER)/apps/cape_mamult.c -o $(EXE_FOLDER)/cape_mamult $(LDFLAGS)

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

cape_test_heap:
	$(COMPILE) $(C_FOLDER)/apps/cape_test_heap.c -o $(EXE_FOLDER)/cape_test_heap $(LDFLAGS)

# ── Build all timestamp applications ─────────────────────────────────────────
apps: cape_mamult cape_mamult2d cape_pi cape_prime cape_prime_number \
      cape_reduction cape_for cape_for_nowait cape_parallel_for \
      cape_vector1 cape_vector2 cape_critical cape_critical2 \
      cape_sections_nowait cape_test_heap

# ── DICKPT version (cape_incr.c) ─────────────────────────────────────────────
# cape_incr.c is a standalone ptrace-based monitor with its own main().
# It forks and traces the application at runtime.
#   make dickpt               — builds bin/cape_dickpt_monitor
#   Usage: srun bin/cape_dickpt_monitor <app_binary>
dickpt:
	$(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape_incr.c -o $(EXE_FOLDER)/cape_dickpt_monitor $(LDFLAGS)

# ── Clean ─────────────────────────────────────────────────────────────────────
cleanall:
	rm -f $(EXE_FOLDER)/* $(O_FOLDER)/* $(L_FOLDER)/*

.PHONY: all apps cleanall dickpt \
        cape_mamult cape_mamult2d cape_pi cape_prime cape_prime_number \
        cape_reduction cape_for cape_for_nowait cape_parallel_for \
        cape_vector1 cape_vector2 cape_critical cape_critical2 \
        cape_sections_nowait cape_test_heap
