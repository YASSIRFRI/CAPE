#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 1024
#define MAX_N 3000

/* Only the result matrix c needs to be in the tracked region for checkpointing.
 * a and b are read-only during computation — keep them as globals to avoid
 * unnecessary write-protection overhead. Loop variables are local.
 *
 * With ASLR disabled (cape_dickpt_runtime.c constructor) and -no-pie, all
 * globals land at the same VA on every rank, so we can register c directly
 * instead of mapping it via dickpt_map_region. */
static int a[MAX_N][MAX_N];
static int b[MAX_N][MAX_N];

struct ckpt_state {
	int c[MAX_N][MAX_N];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

/* Verify the FULL result matrix (all rows, not just local).
 * After allreduce every node should have the complete c = a * b. */
static int verify_full(const struct ckpt_state *state, int n,
		       int row_start, int row_end,
		       unsigned long node, int rep)
{
	int errors = 0;
	int i, j, k;

	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			long long expected = 0;
			for (k = 0; k < n; k++)
				expected += (long long)a[i][k] * b[k][j];

			if ((long long)state->c[i][j] != expected) {
				const char *origin = (i >= row_start && i < row_end)
						     ? "LOCAL" : "REMOTE";
				fprintf(stderr,
					"VERIFY FAIL [%s] node=%lu rep=%d "
					"c[%d][%d] got=%d expected=%lld\n",
					origin, node, rep, i, j,
					state->c[i][j], expected);
				if (++errors >= 10) {
					fprintf(stderr,
						"... too many errors, stopping\n");
					return errors;
				}
			}
		}
	}

	if (errors == 0 && node == 0)
		printf("VERIFY OK  rep=%d (%d x %d) all %d cells correct\n",
		       rep, n, n, n * n);

	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep;
	unsigned long t0, t1;
	struct ckpt_state *state;
	unsigned long node, num_nodes;
	int i, j, k, sum;
	int row_start, row_end;

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

	state = &g_state;
	memset(state, 0, sizeof(*state));
	dickpt_register_region(state, sizeof(*state));

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();
	dickpt_send_num_jobs(n);

	/* Compute row range for this rank */
	row_start = (n * node) / num_nodes;
	row_end = (n * (node + 1)) / num_nodes;

	/* Initialize matrices */
	srand(12345);
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			a[i][j] = rand() % 100;
			b[i][j] = rand() % 100;
			state->c[i][j] = 0;
		}
	}

	for (rep = 1; rep <= reps; rep++) {
		/* Reset c */
		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				state->c[i][j] = 0;

		t0 = get_ms_of_day();

		dickpt_start_ckpt();

		for (i = row_start; i < row_end; i++) {
			for (j = 0; j < n; j++) {
				sum = 0;
				for (k = 0; k < n; k++)
					sum += a[i][k] * b[k][j];
				state->c[i][j] = sum;
			}
		}

		dickpt_generate_ckpt();
		dickpt_allreduce_ckpt();
		dickpt_stop_ckpt();

		t1 = get_ms_of_day();
		if (verify_full(state, n, row_start, row_end, node, rep) != 0)
			return 1;

		if (node == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	return 0;
}
