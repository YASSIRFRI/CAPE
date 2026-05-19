/*
 * write_sparce (CAPE library version, bitmap backend):
 *   A 2D shared grid is declared as one tracked region. Each phase
 *   each rank applies a *small* number of random updates (sparse) to
 *   its own region of the grid; cape_end aggregates with allreduce.
 *
 *   This exercises the bitmap backend's ability to capture only the
 *   pages that actually got touched, in contrast with write_stress
 *   which dirties every page every phase.
 *
 *   Args: <n_cells_per_row> <updates_per_phase> <phases> <reps>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../include/cape.h"

#define DEFAULT_N        (1U << 20)   /* 4 MiB / row at 4B/cell */
#define MAX_N            (1U << 20)
#define MAX_NODES        32
#define DEFAULT_UPDATES  5000
#define MAX_UPDATES      (1U << 20)
#define DEFAULT_PHASES   1
#define MAX_PHASES       32

unsigned int mem_rows[MAX_NODES][MAX_N] __attribute__((aligned(4096)));

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static unsigned long get_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000000UL + tv.tv_usec);
}

void *__get_pc(void) { return __builtin_return_address(0); }

static uint64_t next_rand64(uint64_t *state)
{
	uint64_t x = *state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * 2685821657736338717ULL;
}

static uint64_t seed_for(unsigned int rank, int rep, int phase)
{
	return 0x6a09e667f3bcc909ULL ^
	       ((uint64_t)(rank + 1U) * 0xbf58476d1ce4e5b9ULL) ^
	       ((uint64_t)rep         * 0x94d049bb133111ebULL) ^
	       ((uint64_t)(phase + 1) * 0xd1b54a32d192ed03ULL);
}

/* Sparse XOR-update kernel. Touches `updates` random cells per call;
 * returns the XOR-accumulator of all values written so verify can
 * predict the final checksum without sharing state. */
static unsigned int run_sparse_phase(unsigned int *row, int n, int updates,
				     unsigned int rank, int rep, int phase,
				     int do_writes)
{
	uint64_t rng = seed_for(rank, rep, phase);
	unsigned int expected_xor = 0;
	int t;

	for (t = 0; t < updates; t++) {
		uint64_t r = next_rand64(&rng);
		size_t idx = (size_t)(r % (uint64_t)n);
		unsigned int value =
			(unsigned int)(next_rand64(&rng) ^ (r >> 32) ^ (uint64_t)t);

		if (do_writes)
			row[idx] ^= value;
		expected_xor ^= value;
	}

	return expected_xor;
}

static unsigned int checksum_row(const unsigned int *row, int n)
{
	unsigned int x = 0;
	int i;

	for (i = 0; i < n; i++)
		x ^= row[i];

	return x;
}

static void zero_state(int n, int num_nodes)
{
	int r;

	for (r = 0; r < num_nodes; r++)
		memset(mem_rows[r], 0, (size_t)n * sizeof(mem_rows[r][0]));
}

static unsigned long phase_begin_us, phase_ckpt_us, phase_work_us, phase_end_us;

static void sparce_phase(int n, int updates, int rep, int phase)
{
	__enter_func();
	int node;
	unsigned long t0, t1, t2, t3, t4;

	t0 = get_us();
	cape_begin(PARALLEL_FOR, 0, n);
	t1 = get_us();
	ckpt_start();
	t2 = get_us();
	node = cape_get_node_num();
	run_sparse_phase(mem_rows[node], n, updates,
			 (unsigned int)node, rep, phase, 1);
	t3 = get_us();
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL_FOR, FALSE);
	t4 = get_us();

	phase_begin_us += t1 - t0;
	phase_ckpt_us  += t2 - t1;
	phase_work_us  += t3 - t2;
	phase_end_us   += t4 - t3;

	__exit_func();
}

static int verify(int n, int updates, int num_nodes, unsigned long node,
		  int rep, int phases)
{
	int errors = 0;
	int r, p;

	for (r = 0; r < num_nodes; r++) {
		unsigned int got = checksum_row(mem_rows[r], n);
		unsigned int expected = 0;

		for (p = 0; p < phases; p++)
			expected ^= run_sparse_phase(NULL, n, updates,
						     (unsigned int)r, rep, p, 0);

		if (got != expected) {
			fprintf(stderr,
				"VERIFY FAIL node=%lu rep=%d row=%d got=0x%08x expected=0x%08x\n",
				node, rep, r, got, expected);
			if (++errors >= 10)
				return errors;
		}
	}

	if (errors == 0 && node == 0)
		printf("VERIFY OK  rep=%d (%d rows x %d cells, %d phases, %d sparse updates/phase)\n",
		       rep, num_nodes, n, phases, updates);
	return errors;
}

int main(int argc, char **argv)
{
	int n = DEFAULT_N;
	int updates = DEFAULT_UPDATES;
	int phases = DEFAULT_PHASES;
	int reps = 1;
	int rep, p;
	int num_nodes;
	unsigned long node;
	unsigned long t0, t1;

	if (argc > 1) n       = atoi(argv[1]);
	if (argc > 2) updates = atoi(argv[2]);
	if (argc > 3) phases  = atoi(argv[3]);
	if (argc > 4) reps    = atoi(argv[4]);
	if (n <= 0 || n > (int)MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %u], got %d\n", MAX_N, n);
		return 1;
	}
	if (updates <= 0 || updates > (int)MAX_UPDATES) {
		fprintf(stderr, "ERROR: updates must be in [1, %u], got %d\n",
			MAX_UPDATES, updates);
		return 1;
	}
	if (phases <= 0 || phases > MAX_PHASES) {
		fprintf(stderr, "ERROR: phases must be in [1, %d], got %d\n",
			MAX_PHASES, phases);
		return 1;
	}
	if (reps <= 0) reps = 1;

	cape_declare_variable(&mem_rows, CAPE_UNSIGNED_INT,
			      MAX_NODES * MAX_N, 0);
	cape_init();

	node = (unsigned long)cape_get_node_num();
	num_nodes = cape_get_num_nodes();
	if (num_nodes <= 0 || num_nodes > MAX_NODES) {
		fprintf(stderr, "ERROR: num_nodes must be in [1, %d], got %d\n",
			MAX_NODES, num_nodes);
		cape_finalize();
		return 1;
	}

	for (rep = 1; rep <= reps; rep++) {
		zero_state(n, num_nodes);
		phase_begin_us = phase_ckpt_us = phase_work_us = phase_end_us = 0;
		t0 = get_ms_of_day();
		for (p = 0; p < phases; p++)
			sparce_phase(n, updates, rep, p);
		t1 = get_ms_of_day();

		if (verify(n, updates, num_nodes, node, rep, phases) != 0) {
			cape_finalize();
			return 1;
		}
		if (node == 0) {
			printf("RESULT n=%d updates=%d phases=%d rep=%d ms=%lu\n",
			       n, updates, phases, rep, t1 - t0);
			printf("BREAKDOWN rep=%d (us, sum across %d phases) "
			       "cape_begin=%lu ckpt_start=%lu user_work=%lu cape_end=%lu\n",
			       rep, phases,
			       phase_begin_us, phase_ckpt_us,
			       phase_work_us, phase_end_us);
		}
	}

	cape_finalize();
	return 0;
}
