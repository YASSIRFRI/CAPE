/*
 * write_stress (DICKPT version):
 *   Per-rank row in a 2D state. Each phase: zero the entire state
 *   (outside the ckpt window), then start_ckpt → scattered random
 *   writes into own row → generate/allreduce/stop. Zeroing the state
 *   each phase keeps the allreduce semantics matching memwrite's
 *   single-phase case (only one rank dirties any given row).
 *   MAX_N kept modest so the registered region (and per-phase ckpt
 *   buffer leak in the monitor) stays within per-task memory limits.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N       (1U << 19)
#define MAX_N           (1U << 19)
#define MAX_NODES       32
#define DEFAULT_PHASES  8
#define MAX_PHASES      32
#define WRITES_PER_CELL 4U

struct ckpt_state {
	unsigned int rows[MAX_NODES][MAX_N] __attribute__((aligned(4096)));
};

static struct ckpt_state g_state;

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

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

static void zero_state(struct ckpt_state *state, int n,
		       unsigned long num_nodes)
{
	unsigned long r;

	for (r = 0; r < num_nodes; r++)
		memset(state->rows[r], 0,
		       (size_t)n * sizeof(state->rows[r][0]));
}

static int verify(const struct ckpt_state *state, int n,
		  unsigned long num_nodes, unsigned long node, int rep,
		  int phase)
{
	int errors = 0;
	unsigned long r;

	for (r = 0; r < num_nodes; r++) {
		unsigned int got = checksum_row(state->rows[r], n);
		unsigned int expected =
			run_phase(NULL, n, (unsigned int)r, rep, phase, 0);

		if (got != expected) {
			fprintf(stderr,
				"VERIFY FAIL node=%lu rep=%d row=%lu phase=%d got=0x%08x expected=0x%08x\n",
				node, rep, r, phase, got, expected);
			if (++errors >= 10)
				return errors;
		}
	}

	if (errors == 0 && node == 0)
		printf("VERIFY OK  rep=%d phase=%d (%lu rows x %d cells, %u writes/cell/phase)\n",
		       rep, phase, num_nodes, n, WRITES_PER_CELL);
	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int phases = DEFAULT_PHASES;
	int reps = 1;
	int rep, p;
	struct ckpt_state *state = &g_state;
	unsigned long node, num_nodes;
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

	dickpt_register_region(state, sizeof(*state));

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();
	if (num_nodes == 0 || num_nodes > MAX_NODES) {
		fprintf(stderr, "ERROR: num_nodes must be in [1, %d], got %lu\n",
			MAX_NODES, num_nodes);
		return 1;
	}
	dickpt_send_num_jobs(n);

	for (rep = 1; rep <= reps; rep++) {
		t0 = get_ms_of_day();

		for (p = 0; p < phases; p++) {
			zero_state(state, n, num_nodes);
			dickpt_start_ckpt();
			run_phase(state->rows[node], n,
				  (unsigned int)node, rep, p, 1);
			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();
		}

		t1 = get_ms_of_day();

		if (verify(state, n, num_nodes, node, rep, phases - 1) != 0)
			return 1;

		if (node == 0)
			printf("RESULT n=%d phases=%d rep=%d ms=%lu\n",
			       n, phases, rep, t1 - t0);
	}

	return 0;
}
