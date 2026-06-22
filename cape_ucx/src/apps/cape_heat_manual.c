/*
 * 2D heat-diffusion / Laplace solver (Jacobi iteration) — DICKPT version.
 *
 * This is the canonical iterative-stencil HPC benchmark. We solve the steady
 * state of the heat equation on an N x N grid with Dirichlet boundaries (a hot
 * top edge, cold elsewhere) by repeated Jacobi sweeps:
 *
 *     u'[i][j] = 0.25 * (u[i-1][j] + u[i+1][j] + u[i][j-1] + u[i][j+1])
 *
 * Why this captures the power of DICKPT
 * -------------------------------------
 * The grid is row-block distributed. Each rank updates ONLY its own stripe of
 * rows, reading its neighbours' boundary ("halo") rows that were produced in
 * the previous iteration. In a classic MPI code this forces the programmer to
 * hand-write a halo exchange (MPI_Sendrecv with up/down neighbours) every
 * single iteration — see mpi_heat.c.
 *
 * Here the application is written as if it were plain shared-memory OpenMP: it
 * just writes its rows of the shared grid and lets the DICKPT monitor figure
 * out what changed. Per iteration only the dirty stripe is captured and merged
 * (incremental checkpoint + union allreduce). No explicit message passing, no
 * neighbour bookkeeping, no ghost-cell buffers — the runtime transparently
 * communicates exactly the deltas. That is the whole point of DICKPT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 2048
#define DEFAULT_ITERS 100
#define MAX_N 4096

#define HOT 1.0
#define COLD 0.0

/* The shared grid is the only checkpoint-tracked state. Each rank writes only
 * its own row stripe, so the per-iteration deltas are disjoint and the
 * allreduce merge is a plain union. */
struct ckpt_state {
	double u[MAX_N][MAX_N];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

/* Local double-buffer scratch — never shipped, so it stays untracked. */
static double unew[MAX_N][MAX_N];

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

/* Identical initial/boundary condition on every rank: hot top edge. */
static void init_grid(struct ckpt_state *s, int n)
{
	int i, j;
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			s->u[i][j] = COLD;
	for (j = 0; j < n; j++)
		s->u[0][j] = HOT;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int iters = DEFAULT_ITERS;
	int reps = 1;
	int rep;
	unsigned long node, num_nodes;
	int r_start, r_end;          /* this rank's global row stripe */
	int i_lo, i_hi;              /* interior rows this rank updates */
	struct ckpt_state *state = &g_state;

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		iters = atoi(argv[2]);
	if (argc > 3)
		reps = atoi(argv[3]);
	if (n <= 2 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [3, %d], got %d\n", MAX_N, n);
		return 1;
	}
	if (iters <= 0)
		iters = 1;
	if (reps <= 0)
		reps = 1;

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();

	r_start = (int)(((long)n * node) / num_nodes);
	r_end = (int)(((long)n * (node + 1)) / num_nodes);
	/* Global rows 0 and n-1 are fixed Dirichlet boundaries. */
	i_lo = (r_start < 1) ? 1 : r_start;
	i_hi = (r_end > n - 1) ? n - 1 : r_end;

	dickpt_register_region(state, sizeof(*state));
	dickpt_send_num_jobs((unsigned long)n);

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		int it, i, j;

		init_grid(state, n);

		t0 = get_ms_of_day();

		for (it = 0; it < iters; it++) {
			dickpt_start_ckpt();

			/* Jacobi sweep over this rank's interior rows. Reads the
			 * neighbours' halo rows produced by the previous merge. */
			for (i = i_lo; i < i_hi; i++)
				for (j = 1; j < n - 1; j++)
					unew[i][j] = 0.25 *
						(state->u[i - 1][j] +
						 state->u[i + 1][j] +
						 state->u[i][j - 1] +
						 state->u[i][j + 1]);

			/* Commit our stripe into the shared grid (the dirty set). */
			for (i = i_lo; i < i_hi; i++)
				for (j = 1; j < n - 1; j++)
					state->u[i][j] = unew[i][j];

			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();
		}

		t1 = get_ms_of_day();

		if (node == 0) {
			double sum = 0.0;
			for (i = 1; i < n - 1; i++)
				for (j = 1; j < n - 1; j++)
					sum += state->u[i][j];
			printf("VERIFY OK  rep=%d iters=%d avg_interior=%.8f center=%.8f\n",
			       rep, iters, sum / ((double)(n - 2) * (n - 2)),
			       state->u[n / 2][n / 2]);
			printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
			       n, iters, rep, t1 - t0);
			fflush(stdout);
		}
	}

	return 0;
}
