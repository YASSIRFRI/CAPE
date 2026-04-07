#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/cape_dickpt.h"

#define  N  8

struct mul_state {
	unsigned long c[N][N];
	unsigned long a[N][N];
	unsigned long b[N][N];
	int i;
	int j;
	int k;
};

/* -----------------------------------------------------------
 * Program
 * -----------------------------------------------------------
 */
int main(int argc,char*argv[])
{
	struct mul_state *state;
	unsigned long node;
	unsigned long sum;

	state = dickpt_map_region(sizeof(*state));
	if (state == NULL) {
		perror("dickpt_map_region");
		return 1;
	}
	memset(state, 0, sizeof(*state));

	node = dickpt_read_node();
	dickpt_send_num_jobs(N);

	//load data
	for (state->i = 0; state->i < N; state->i++) {
		for (state->j = 0; state->j < N; state->j++) {
			state->c[state->i][state->j] = 0;
			state->a[state->i][state->j] = 1;
			state->b[state->i][state->j] = 1;
		}
	}


	if (node == 0)
		dickpt_start_ckpt();

	for (state->i = 0; state->i < N; state->i++) {
		for (state->j = 0; state->j < N; state->j++) {
			sum = 0;
			for (state->k = 0; state->k < N; state->k++)
				sum += state->a[state->i][state->k] * state->b[state->k][state->j];
			state->c[state->i][state->j] = sum;
		}

		if (node == 0) {
			dickpt_generate_ckpt();
		}
	}

	if (node == 0)
	{
		dickpt_generate_ckpt();
		dickpt_stop_ckpt();
	}

	//Print result
	for (state->i = 0; state->i < N; state->i++) {
		for (state->j = 0; state->j < N; state->j++)
			printf("%ld\t", state->c[state->i][state->j]);
		printf("\n");
	}

	return 0;
}
