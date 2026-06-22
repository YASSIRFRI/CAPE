/*
 * Distributed dense matrix multiplication C = A x B — DICKPT version.
 *
 * The ultimate compute-bound benchmark: an N^3 inner loop with no
 * communication until the final exchange. Rows of C are block-distributed
 * across ranks; A and B are replicated read-only globals (same seed on every
 * rank), so they are NOT in the tracked region. Each rank computes its own
 * stripe of rows and writes them into the shared C; one allreduce unions the
 * disjoint row stripes (block pattern), after which every rank holds the full
 * product. Uses double precision so the FLOP-heavy core dominates the runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 1024
#define MAX_N 2048

/* Read-only operands, replicated on every rank — not tracked. */
static double a[MAX_N][MAX_N];
static double b[MAX_N][MAX_N];

/* Result matrix: each rank writes only its row stripe (disjoint union). */
struct ckpt_state {
	double c[MAX_N][MAX_N];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static unsigned long long next_rand64(unsigned long long *state)
{
	unsigned long long x = *state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * 2685821657736338717ULL;
}

static double to_unit(unsigned long long r)
{
	return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

/* Spot-check a few cells of the local stripe against a recomputed reference. */
static int verify_sample(const struct ckpt_state *s, int n,
			 int row_start, int row_end, unsigned long node, int rep)
{
	int checks[4][2];
	int span = (row_end > row_start) ? (row_end - row_start) : 1;
	int t;

	checks[0][0] = row_start;        checks[0][1] = 0;
	checks[1][0] = row_start;        checks[1][1] = n - 1;
	checks[2][0] = row_end - 1;      checks[2][1] = n / 2;
	checks[3][0] = row_start + span / 2; checks[3][1] = n / 3;

	for (t = 0; t < 4; t++) {
		int i = checks[t][0], j = checks[t][1], k;
		double expected = 0.0;
		if (i < 0 || i >= n || j < 0 || j >= n)
			continue;
		for (k = 0; k < n; k++)
			expected += a[i][k] * b[k][j];
		if (fabs(s->c[i][j] - expected) > 1e-6 * (fabs(expected) + 1.0)) {
			fprintf(stderr,
				"VERIFY FAIL node=%lu rep=%d c[%d][%d] got=%.6f expected=%.6f\n",
				node, rep, i, j, s->c[i][j], expected);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep;
	unsigned long node, num_nodes;
	int row_start, row_end;
	struct ckpt_state *state = &g_state;
	unsigned long long rng = 0xdeadbeefcafebabeULL;
	int i, j, k;

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		reps = atoi(argv[2]);
	if (n <= 0 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n", MAX_N, n);
		return 1;
	}
	if (reps <= 0)
		reps = 1;

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();

	row_start = (int)(((long)n * node) / num_nodes);
	row_end = (int)(((long)n * (node + 1)) / num_nodes);

	/* Same seed on every rank => identical replicated A and B. */
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++) {
			a[i][j] = to_unit(next_rand64(&rng));
			b[i][j] = to_unit(next_rand64(&rng));
		}

	memset(state, 0, sizeof(*state));
	dickpt_register_region(state, sizeof(*state));
	dickpt_send_num_jobs((unsigned long)n);

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;

		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				state->c[i][j] = 0.0;

		t0 = get_ms_of_day();
		dickpt_start_ckpt();

		for (i = row_start; i < row_end; i++) {
			for (k = 0; k < n; k++) {
				double aik = a[i][k];
				for (j = 0; j < n; j++)
					state->c[i][j] += aik * b[k][j];
			}
		}

		dickpt_generate_ckpt();
		dickpt_allreduce_ckpt();
		dickpt_stop_ckpt();

		t1 = get_ms_of_day();

		if (verify_sample(state, n, row_start, row_end, node, rep) != 0)
			return 1;

		if (node == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	return 0;
}
