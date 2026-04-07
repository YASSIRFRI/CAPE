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
};

/* -----------------------------------------------------------
 * Program
 * -----------------------------------------------------------
 */
int main(int argc,char*argv[])
{
	struct mul_state *state;
	unsigned long node;

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

	for (state->i = 0; state->i < N; state->i++)
	{
		if (node == 0)
		{
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
