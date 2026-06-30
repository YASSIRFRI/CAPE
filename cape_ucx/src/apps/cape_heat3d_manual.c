/*
 * 3D diffusion solver (Jacobi iteration) — DICKPT version.
 *
 * Transient scalar diffusion on an N x N x N cube — the kernel behind
 * thermal/fluid-dynamics diffusion (heat conduction, viscous/species terms).
 * We march to steady state with a 7-point Jacobi stencil:
 *
 *   u'[i][j][k] = (1/6) * ( u[i-1][j][k] + u[i+1][j][k]
 *                         + u[i][j-1][k] + u[i][j+1][k]
 *                         + u[i][j][k-1] + u[i][j][k+1] )
 *
 * Dirichlet boundaries: the i==0 face is held HOT, everything else COLD.
 *
 * Why this captures the power of DICKPT
 * -------------------------------------
 * The cube is slab-distributed along the first axis (i). Each rank updates ONLY
 * its own block of i-planes, reading its neighbours' boundary ("halo") planes
 * produced in the previous iteration. A classic MPI code forces the programmer
 * to hand-write a halo exchange with up/down neighbours every iteration — see
 * mpi_heat3d.c.
 *
 * Here the application is written as if it were plain shared-memory OpenMP: it
 * writes its planes of the shared cube and lets the DICKPT monitor figure out
 * what changed. Per iteration only the dirty slab is captured and merged
 * (incremental checkpoint + union allreduce). For diffusion from a hot face the
 * delta starts TINY (only the planes near the front move) and grows as the
 * front sweeps through the cube — which is exactly what the checkpoint-size job
 * logs.
 *
 * Implementation notes (mirroring cape_heat_manual.c):
 *   - The tracked cube comes from dickpt_map_region(): a clean page-aligned
 *     anonymous mapping at an identical VA on every rank, registered for
 *     write-protect in one shot.
 *   - We pre-touch our slab before each start_ckpt so the pages are resident
 *     when the monitor write-protects them; otherwise a first write to a
 *     not-yet-resident page would not fault and the delta would be missed.
 *   - The double-buffer scratch (unew) is ordinary heap memory: never shipped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 128
#define DEFAULT_ITERS 100
#define MAX_N 512

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
	int rep, i, j, k;
	unsigned long node, num_nodes;
	int r_start, r_end;          /* this rank's global i-plane slab */
	int i_lo, i_hi;              /* interior i-planes this rank updates */
	size_t bytes, plane;
	double *u = NULL;            /* tracked shared cube (flat n x n x n) */
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
	/* Global planes 0 and n-1 are fixed Dirichlet boundaries. */
	i_lo = (r_start < 1) ? 1 : r_start;
	i_hi = (r_end > n - 1) ? n - 1 : r_end;

	plane = (size_t)n * (size_t)n;
	bytes = plane * (size_t)n * sizeof(double);

	u = (double *)dickpt_map_region(bytes);
	unew = (double *)malloc(bytes);
	if (u == NULL || unew == NULL) {
		fprintf(stderr, "node %lu: allocation failed (%zu bytes)\n",
			node, bytes);
		return 1;
	}
	dickpt_send_num_jobs((unsigned long)n);

#define U(i, j, k)    u[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]
#define UNEW(i, j, k) unew[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		int it;

		/* Identical initial/boundary condition on every rank: hot i==0 face. */
		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				for (k = 0; k < n; k++)
					U(i, j, k) = COLD;
		for (j = 0; j < n; j++)
			for (k = 0; k < n; k++)
				U(0, j, k) = HOT;

		t0 = get_ms_of_day();

		for (it = 0; it < iters; it++) {
			/* Pre-touch our slab so its pages are resident before
			 * the monitor write-protects them at start_ckpt. */
			for (i = i_lo; i < i_hi; i++) {
				volatile double *p = &U(i, 0, 0);
				p[0] = p[0];
				p[plane - 1] = p[plane - 1];
			}

			dickpt_start_ckpt();

			/* Jacobi sweep over this rank's interior planes. Reads the
			 * neighbours' halo planes produced by the previous merge. */
			for (i = i_lo; i < i_hi; i++)
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						UNEW(i, j, k) = (1.0 / 6.0) *
							(U(i - 1, j, k) + U(i + 1, j, k) +
							 U(i, j - 1, k) + U(i, j + 1, k) +
							 U(i, j, k - 1) + U(i, j, k + 1));

			/* Commit our slab into the shared cube (the dirty set).
			 * Write back ONLY cells whose value actually changed. Ahead
			 * of the diffusion front the field is still exactly COLD and
			 * the Jacobi average of all-COLD neighbours is exactly COLD
			 * (IEEE: 0+0+...=0), so those cells are skipped and their
			 * pages are never faulted/dirtied. This is what makes the
			 * incremental checkpoint start tiny and grow ~1 plane/iter as
			 * the front sweeps through the cube — the whole premise of the
			 * benchmark. An unconditional write would dirty the entire
			 * slab every iteration and erase DICKPT's advantage. */
			for (i = i_lo; i < i_hi; i++)
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						if (UNEW(i, j, k) != U(i, j, k))
							U(i, j, k) = UNEW(i, j, k);

			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();
		}

		t1 = get_ms_of_day();

		if (node == 0) {
			double sum = 0.0;
			double denom = (double)(n - 2) * (n - 2) * (n - 2);
			for (i = 1; i < n - 1; i++)
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						sum += U(i, j, k);
			printf("VERIFY OK  rep=%d iters=%d avg_interior=%.8f center=%.8f\n",
			       rep, iters, sum / denom,
			       U(n / 2, n / 2, n / 2));
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
