#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

/* Distributed gradient of MSE loss: grad[j] = sum_i X[i][j] * (X[i].w - y[i])
 * Samples are partitioned across ranks; local partial gradients are summed
 * via the monitor's allreduce. */

#define DEFAULT_N 4096
#define DEFAULT_D 256
#define MAX_N 8192
#define MAX_D 1024

static int X[MAX_N][MAX_D];
static int w[MAX_D];
static int y[MAX_N];

struct ckpt_state {
	long long grad[MAX_D];
};
static struct ckpt_state g_state;

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static int verify_full(const struct ckpt_state *state, int n, int d,
		       unsigned long node, int rep)
{
	int errors = 0;
	int i, j;

	for (j = 0; j < d; j++) {
		long long expected = 0;
		for (i = 0; i < n; i++) {
			long long r = -(long long)y[i];
			for (int k = 0; k < d; k++)
				r += (long long)X[i][k] * w[k];
			expected += (long long)X[i][j] * r;
		}
		if (state->grad[j] != expected) {
			fprintf(stderr,
				"VERIFY FAIL node=%lu rep=%d grad[%d] got=%lld expected=%lld\n",
				node, rep, j, state->grad[j], expected);
			if (++errors >= 10) return errors;
		}
	}

	if (errors == 0 && node == 0)
		printf("VERIFY OK  rep=%d (grad length %d) all cells correct\n",
		       rep, d);
	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int d = DEFAULT_D;
	int reps = 1;
	int rep, i, j;
	unsigned long t0, t1;
	struct ckpt_state *state;
	unsigned long node, num_nodes;
	int row_start, row_end;

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) d = atoi(argv[2]);
	if (argc > 3) reps = atoi(argv[3]);
	if (n <= 0 || n > MAX_N || d <= 0 || d > MAX_D) {
		fprintf(stderr,
			"ERROR: need 1<=n<=%d (got %d) and 1<=d<=%d (got %d)\n",
			MAX_N, n, MAX_D, d);
		return 1;
	}
	if (reps <= 0) reps = 1;

	state = &g_state;
	memset(state, 0, sizeof(*state));
	dickpt_register_region(state, sizeof(*state));

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();
	dickpt_send_num_jobs(n);

	row_start = (n * node) / num_nodes;
	row_end = (n * (node + 1)) / num_nodes;

	/* Keep values small so grad magnitudes stay within long long range. */
	srand(12345);
	for (i = 0; i < n; i++) {
		y[i] = rand() % 10;
		for (j = 0; j < d; j++)
			X[i][j] = rand() % 10;
	}
	for (j = 0; j < d; j++)
		w[j] = rand() % 10;

	for (rep = 1; rep <= reps; rep++) {
		for (j = 0; j < d; j++) state->grad[j] = 0;

		t0 = get_ms_of_day();
		dickpt_start_ckpt();

		for (i = row_start; i < row_end; i++) {
			long long r = -(long long)y[i];
			for (int k = 0; k < d; k++)
				r += (long long)X[i][k] * w[k];
			for (j = 0; j < d; j++)
				state->grad[j] += (long long)X[i][j] * r;
		}

		dickpt_generate_ckpt();
		dickpt_allreduce_ckpt();
		dickpt_stop_ckpt();

		t1 = get_ms_of_day();
		if (verify_full(state, n, d, node, rep) != 0) return 1;
		if (node == 0)
			printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
			       n, d, rep, t1 - t0);
	}

	return 0;
}
