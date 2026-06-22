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
 *
 * Implementation notes (mirroring the proven iterative app cape_game_of_life):
 *   - The tracked grid is obtained from dickpt_map_region(), which returns a
 *     clean page-aligned anonymous mapping at an identical VA on every rank.
 *     This is the supported path for large iterative regions: the whole range
 *     registers for write-protect in one shot (no BSS/.got page-skip fallback).
 *   - We pre-touch our stripe before each start_ckpt so the pages are resident
 *     when the monitor write-protects them; otherwise a write to a not-yet-
 *     resident page would not fault and the delta would be missed.
 *   - The double-buffer scratch (unew) is ordinary heap memory: it is never
 *     shipped, so it stays out of the tracked region.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 2048
#define DEFAULT_ITERS 100
#define MAX_N 8192

#define HOT 1.0
#define COLD 0.0

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int iters = DEFAULT_ITERS;
	int reps = 1;
	int rep, i, j;
	unsigned long node, num_nodes;
	int r_start, r_end;          /* this rank's global row stripe */
	int i_lo, i_hi;              /* interior rows this rank updates */
	size_t bytes;
	double *u = NULL;            /* tracked shared grid (flat n x n) */
	double *unew = NULL;         /* untracked scratch */

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

	bytes = (size_t)n * (size_t)n * sizeof(double);

	u = (double *)dickpt_map_region(bytes);
	unew = (double *)malloc(bytes);
	if (u == NULL || unew == NULL) {
		fprintf(stderr, "node %lu: allocation failed (%zu bytes)\n",
			node, bytes);
		return 1;
	}
	dickpt_send_num_jobs((unsigned long)n);

#define U(i, j)    u[(size_t)(i) * (size_t)n + (size_t)(j)]
#define UNEW(i, j) unew[(size_t)(i) * (size_t)n + (size_t)(j)]

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		int it;

		/* Identical initial/boundary condition on every rank: hot top. */
		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				U(i, j) = COLD;
		for (j = 0; j < n; j++)
			U(0, j) = HOT;

		t0 = get_ms_of_day();

		for (it = 0; it < iters; it++) {
			/* Pre-touch our stripe so its pages are resident before
			 * the monitor write-protects them at start_ckpt. */
			for (i = i_lo; i < i_hi; i++) {
				volatile double *row = &U(i, 0);
				row[0] = row[0];
				row[n - 1] = row[n - 1];
			}

			dickpt_start_ckpt();

			/* Jacobi sweep over this rank's interior rows. Reads the
			 * neighbours' halo rows produced by the previous merge. */
			for (i = i_lo; i < i_hi; i++)
				for (j = 1; j < n - 1; j++)
					UNEW(i, j) = 0.25 *
						(U(i - 1, j) + U(i + 1, j) +
						 U(i, j - 1) + U(i, j + 1));

			/* Commit our stripe into the shared grid (the dirty set). */
			for (i = i_lo; i < i_hi; i++)
				for (j = 1; j < n - 1; j++)
					U(i, j) = UNEW(i, j);

			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();
		}

		t1 = get_ms_of_day();

		if (node == 0) {
			double sum = 0.0;
			for (i = 1; i < n - 1; i++)
				for (j = 1; j < n - 1; j++)
					sum += U(i, j);
			printf("VERIFY OK  rep=%d iters=%d avg_interior=%.8f center=%.8f\n",
			       rep, iters, sum / ((double)(n - 2) * (n - 2)),
			       U(n / 2, n / 2));
			printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
			       n, iters, rep, t1 - t0);
			fflush(stdout);
		}
	}

#undef U
#undef UNEW
	free(unew);
	return 0;
}
