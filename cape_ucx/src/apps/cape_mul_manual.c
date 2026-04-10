#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 1024
#define MAX_N 3200

struct mul_state {
	int c[MAX_N][MAX_N];
	int a[MAX_N][MAX_N];
	int b[MAX_N][MAX_N];
	int i;
	int j;
	int k;
};

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep;
	unsigned long t0, t1;
	struct mul_state *state;
	unsigned long node, num_nodes;
	int sum;
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

	state = dickpt_map_region(sizeof(*state));
	if (state == NULL) {
		perror("dickpt_map_region");
		return 1;
	}
	memset(state, 0, sizeof(*state));

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();
	dickpt_send_num_jobs(n);

	/* Compute row range for this rank */
	row_start = (n * node) / num_nodes;
	row_end = (n * (node + 1)) / num_nodes;

	/* Initialize matrices */
	srand(12345);
	for (state->i = 0; state->i < n; state->i++) {
		for (state->j = 0; state->j < n; state->j++) {
			state->a[state->i][state->j] = rand() % 100;
			state->b[state->i][state->j] = rand() % 100;
			state->c[state->i][state->j] = 0;
		}
	}

	for (rep = 1; rep <= reps; rep++) {
		/* Reset c */
		for (state->i = 0; state->i < n; state->i++)
			for (state->j = 0; state->j < n; state->j++)
				state->c[state->i][state->j] = 0;

		t0 = get_ms_of_day();

		dickpt_start_ckpt();

		for (state->i = row_start; state->i < row_end; state->i++) {
			for (state->j = 0; state->j < n; state->j++) {
				sum = 0;
				for (state->k = 0; state->k < n; state->k++)
					sum += state->a[state->i][state->k]
					     * state->b[state->k][state->j];
				state->c[state->i][state->j] = sum;
			}

			dickpt_generate_ckpt();
		}

		dickpt_generate_ckpt();
		dickpt_stop_ckpt();

		t1 = get_ms_of_day();

		if (node == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	return 0;
}
