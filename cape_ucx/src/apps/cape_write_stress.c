/*
 * write_stress (CAPE timestamp version):
 *   Per-rank row in a shared 2D state. Each phase: zero the entire
 *   state (so only the active rank's writes are nonzero in the
 *   checkpoint window), run scattered random writes against rank+phase
 *   seed, cape_end() aggregates. Multiple phases stress per-checkpoint
 *   synchronization and incremental tracking.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../include/cape.h"

#define DEFAULT_N       (1U << 20)
#define MAX_N           (1U << 20)
#define MAX_NODES       32
#define DEFAULT_PHASES  8
#define MAX_PHASES      32
#define WRITES_PER_CELL 4U

unsigned int mem_rows[MAX_NODES][MAX_N] __attribute__((aligned(4096)));

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
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
	return 0x9e3779b97f4a7c15ULL ^
	       ((uint64_t)(rank + 1U) * 0xbf58476d1ce4e5b9ULL) ^
	       ((uint64_t)rep   * 0x94d049bb133111ebULL) ^
	       ((uint64_t)(phase + 1) * 0xd1b54a32d192ed03ULL);
}

static unsigned int run_phase(unsigned int *row, int n,
			      unsigned int rank, int rep, int phase,
			      int do_writes)
{
	uint64_t rng = seed_for(rank, rep, phase);
	size_t writes = (size_t)n * WRITES_PER_CELL;
	unsigned int expected_xor = 0;
	size_t t;

	for (t = 0; t < writes; t++) {
		uint64_t r = next_rand64(&rng);
		size_t idx = (size_t)(r % (uint64_t)n);
		unsigned int value =
			(unsigned int)(next_rand64(&rng) ^ (r >> 32) ^ t);

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

static void write_stress_phase(int n, int rep, int phase)
{
	__enter_func();
	int node;

	cape_begin(PARALLEL_FOR, 0, n);
	ckpt_start();
	node = cape_get_node_num();
	run_phase(mem_rows[node], n, (unsigned int)node, rep, phase, 1);
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL_FOR, FALSE);
	__exit_func();
}

static int verify(int n, int num_nodes, unsigned long node, int rep,
		  int phases)
{
	int errors = 0;
	int r, p;

	for (r = 0; r < num_nodes; r++) {
		unsigned int got = checksum_row(mem_rows[r], n);
		unsigned int expected = 0;

		for (p = 0; p < phases; p++)
			expected ^= run_phase(NULL, n,
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
		printf("VERIFY OK  rep=%d (%d rows x %d cells, %d phases, %u writes/cell/phase)\n",
		       rep, num_nodes, n, phases, WRITES_PER_CELL);
	return errors;
}

int main(int argc, char **argv)
{
	int n = DEFAULT_N;
	int phases = DEFAULT_PHASES;
	int reps = 1;
	int rep, p;
	int num_nodes;
	unsigned long node;
	unsigned long t0, t1;

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) phases = atoi(argv[2]);
	if (argc > 3) reps = atoi(argv[3]);
	if (n <= 0 || n > (int)MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %u], got %d\n",
			MAX_N, n);
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
		/* Zero once per rep, NOT per phase. Per-phase zeroing
		 * touches every cape-tracked page, which traps through
		 * userfaultfd → ~256K monitor round-trips that swamp the
		 * actual work. cape_end's timestamp-based merge already
		 * propagates per-cell writes across phases correctly. */
		zero_state(n, num_nodes);
		t0 = get_ms_of_day();
		for (p = 0; p < phases; p++)
			write_stress_phase(n, rep, p);
		t1 = get_ms_of_day();

		if (verify(n, num_nodes, node, rep, phases) != 0) {
			cape_finalize();
			return 1;
		}
		if (node == 0)
			printf("RESULT n=%d phases=%d rep=%d ms=%lu\n",
			       n, phases, rep, t1 - t0);
	}

	cape_finalize();
	return 0;
}
