#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 2048
#define MAX_N 4096

static int A[MAX_N][MAX_N];
static int x[MAX_N];

struct ckpt_state {
	int y[MAX_N];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static int verify_full(const struct ckpt_state *state, int n,
		       unsigned long node, int rep)
{
	int errors = 0;
	int i, k;

	for (i = 0; i < n; i++) {
		long long expected = 0;
		for (k = 0; k < n; k++)
			expected += (long long)A[i][k] * x[k];
		if ((long long)state->y[i] != expected) {
			fprintf(stderr,
				"VERIFY FAIL node=%lu rep=%d y[%d] got=%d expected=%lld\n",
				node, rep, i, state->y[i], expected);
			if (++errors >= 10) return errors;
		}
	}

	if (errors == 0 && node == 0)
		printf("VERIFY OK  rep=%d (y length %d) all cells correct\n", rep, n);
	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep, i, k;
	unsigned long t0, t1;
	struct ckpt_state *state;
	unsigned long node, num_nodes;
	int row_start, row_end;

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) reps = atoi(argv[2]);
	if (n <= 0 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n", MAX_N, n);
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

	srand(12345);
	for (i = 0; i < n; i++) {
		x[i] = rand() % 100;
		for (k = 0; k < n; k++)
			A[i][k] = rand() % 100;
	}

	for (rep = 1; rep <= reps; rep++) {
		for (i = 0; i < n; i++) state->y[i] = 0;

		t0 = get_ms_of_day();
		dickpt_start_ckpt();

		for (i = row_start; i < row_end; i++) {
			long long sum = 0;
			for (k = 0; k < n; k++)
				sum += (long long)A[i][k] * x[k];
			state->y[i] = (int)sum;
		}

		dickpt_generate_ckpt();
		dickpt_allreduce_ckpt();
		dickpt_stop_ckpt();

		t1 = get_ms_of_day();
		if (verify_full(state, n, node, rep) != 0) return 1;
		if (node == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	return 0;
}
