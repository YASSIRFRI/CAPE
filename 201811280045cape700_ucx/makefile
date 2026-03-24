O_FOLDER=obj
L_FOLDER=lib
C_FOLDER=src
I_FOLDER=include
EXE_FOLDER=bin
DOCS_FOLDER=docs

# UCX paths — override any of these to match your installation, e.g.:
#   make UCX_SRC=/path/to/ucx/src UCX_GEN=/path/to/ucx/generated
#
# Default: UCX built from source at /usr/local/src/ucx, with the generated
# ucp_version.h living alongside the project libs.
UCX_SRC ?= /usr/local/src/ucx/src
UCX_GEN ?= /home/yassir/internship/code/matmul_bench/ucx_gen
UCX_LIB_DIR ?= /home/yassir/internship/code/matmul_bench/ucx_libs

# PMIx paths (provided by OpenMPI/SLURM).
PMIX_HOME ?= /home/linuxbrew/.linuxbrew

UCX_INC  = -I$(UCX_SRC) -I$(UCX_GEN)
UCX_LIB  = -L$(UCX_LIB_DIR) -lucp -lucs
PMIX_INC = -I$(PMIX_HOME)/include
PMIX_LIB = -L$(PMIX_HOME)/lib -lpmix

CFLAGS  = -I./$(I_FOLDER) $(UCX_INC) $(PMIX_INC)
LDFLAGS = $(UCX_LIB) $(PMIX_LIB) -Wl,-rpath,$(UCX_LIB_DIR) -Wl,-rpath,$(PMIX_HOME)/lib

CC = cc

#Get current folder
PWD = $(shell pwd)

all:
	$(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_mamult.c -o $(EXE_FOLDER)/cape_mamult $(LDFLAGS)
	$(CC) -w -D DDEBUG $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_reduction.c -o $(EXE_FOLDER)/cape_reduction $(LDFLAGS)

cape_mamult: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_mamult.c -o $(EXE_FOLDER)/cape_mamult $(LDFLAGS)

cape_reduction: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w -D DDEBUG $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_reduction.c -o $(EXE_FOLDER)/cape_reduction $(LDFLAGS)

cape_prime: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w -D DDEBUG $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_prime.c -o $(EXE_FOLDER)/cape_prime $(LDFLAGS)

cape_prime_number: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w -D DDEBUG $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_prime_number.c -o $(EXE_FOLDER)/cape_prime_number $(LDFLAGS)

cape_for_nowait: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w -D DDEBUG $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_for_nowait.c -o $(EXE_FOLDER)/cape_for_nowait $(LDFLAGS)

cape_for: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w -D DDEBUG $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_for.c -o $(EXE_FOLDER)/cape_for $(LDFLAGS)

cape_critical: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_critical.c -o $(EXE_FOLDER)/cape_critical $(LDFLAGS)

cape_critical2: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_critical2.c -o $(EXE_FOLDER)/cape_critical2 $(LDFLAGS)

cape_sections_nowait: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w $(CFLAGS) $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_sections_nowait.c -o $(EXE_FOLDER)/cape_sections_nowait $(LDFLAGS)

# Build monitor object (used by the apps target below)
monitor: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	$(CC) -w -c $(CFLAGS) -o $(L_FOLDER)/cape.o $(C_FOLDER)/monitor/cape.c

# Build all apps: compile monitor once, then link each app against it
APPS = cape_mamult cape_mamult2d cape_pi cape_prime cape_prime_number \
       cape_reduction cape_for cape_for_nowait cape_parallel_for \
       cape_vector1 cape_vector2 cape_critical cape_critical2 \
       cape_sections_nowait cape_test_heap

apps: monitor
	@for app in $(APPS); do \
	    src=$(C_FOLDER)/apps/$${app}.c; \
	    [ -f "$$src" ] || continue; \
	    echo "  CC+LD  $$app"; \
	    $(CC) -w $(CFLAGS) -o $(EXE_FOLDER)/$$app \
	        $(L_FOLDER)/cape.o $$src $(LDFLAGS); \
	done

#Clean
cleanall:
	rm -f $(O_FOLDER)/* $(EXE_FOLDER)/* $(L_FOLDER)/*
