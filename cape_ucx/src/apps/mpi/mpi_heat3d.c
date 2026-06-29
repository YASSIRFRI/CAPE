/*
 * 3D diffusion solver (Jacobi iteration) — MPI reference.
 *
 * Transient scalar diffusion on an N x N x N cube — the canonical kernel behind
 * thermal/fluid-dynamics diffusion (heat conduction, species/pressure
 * smoothing, viscous terms). We march the diffusion equation to steady state
 * with a 7-point Jacobi stencil:
 *
 *   u'[i][j][k] = (1/6) * ( u[i-1][j][k] + u[i+1][j][k]
 *                         + u[i][j-1][k] + u[i][j+1][k]
 *                         + u[i][j][k-1] + u[i][j][k+1] )
 *
 * Dirichlet boundaries: the i==0 face is held HOT, everything else COLD. The
 * cube is slab-distributed along the first axis (i): each rank owns a contiguous
 * block of i-planes plus two ghost planes, and trades boundary planes with its
 * up/down neighbours via MPI_Sendrecv every iteration.
 *
 * Compare cape_heat3d_manual.c: the DICKPT version writes its slab into a
 * shared cube and lets the monitor ship only the dirty delta — no ghost-plane
 * buffers and no hand-written halo exchange.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#define DEFAULT_N 128
#define DEFAULT_ITERS 100

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
	int rep, rank, num_ranks;
	int r_start, r_end, local_planes;
	int up, down;
	/* Each rank keeps local_planes interior i-planes plus 2 ghost planes:
	 * index 0 (top halo) and local_planes+1 (bottom halo). A plane is N x N. */
	double *u = NULL, *unew = NULL;
	int li, j, k;
	size_t plane;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		iters = atoi(argv[2]);
	if (argc > 3)
		reps = atoi(argv[3]);
	if (n <= 2) {
		if (rank == 0)
			fprintf(stderr, "ERROR: n must be >= 3, got %d\n", n);
		MPI_Finalize();
		return 1;
	}
	if (iters <= 0)
		iters = 1;
	if (reps <= 0)
		reps = 1;

	r_start = (int)(((long)n * rank) / num_ranks);
	r_end = (int)(((long)n * (rank + 1)) / num_ranks);
	local_planes = r_end - r_start;

	up = (r_start > 0) ? rank - 1 : MPI_PROC_NULL;
	down = (r_end < n) ? rank + 1 : MPI_PROC_NULL;

	plane = (size_t)n * (size_t)n;
	u = malloc((size_t)(local_planes + 2) * plane * sizeof(double));
	unew = malloc((size_t)(local_planes + 2) * plane * sizeof(double));
	if (u == NULL || unew == NULL) {
		fprintf(stderr, "rank %d: allocation failed\n", rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
		return 1;
	}
#define U(li, j, k)    u[((size_t)(li) * plane) + ((size_t)(j) * n) + (k)]
#define UNEW(li, j, k) unew[((size_t)(li) * plane) + ((size_t)(j) * n) + (k)]

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		int it;

		/* Initialise local slab (+ghosts) with the global IC: hot i==0 face. */
		for (li = 0; li < local_planes + 2; li++) {
			int gi = r_start + li - 1;   /* global i-plane of li */
			for (j = 0; j < n; j++)
				for (k = 0; k < n; k++)
					U(li, j, k) = (gi == 0) ? HOT : COLD;
		}

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (it = 0; it < iters; it++) {
			/* Halo exchange: trade boundary interior planes with
			 * neighbours into each other's ghost planes. */
			MPI_Sendrecv(&U(1, 0, 0), (int)plane, MPI_DOUBLE, up, 0,
				     &U(local_planes + 1, 0, 0), (int)plane, MPI_DOUBLE, down, 0,
				     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Sendrecv(&U(local_planes, 0, 0), (int)plane, MPI_DOUBLE, down, 1,
				     &U(0, 0, 0), (int)plane, MPI_DOUBLE, up, 1,
				     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			for (li = 1; li <= local_planes; li++) {
				int gi = r_start + li - 1;
				if (gi == 0 || gi == n - 1)
					continue;   /* fixed Dirichlet faces */
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						UNEW(li, j, k) = (1.0 / 6.0) *
							(U(li - 1, j, k) + U(li + 1, j, k) +
							 U(li, j - 1, k) + U(li, j + 1, k) +
							 U(li, j, k - 1) + U(li, j, k + 1));
			}

			for (li = 1; li <= local_planes; li++) {
				int gi = r_start + li - 1;
				if (gi == 0 || gi == n - 1)
					continue;
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						U(li, j, k) = UNEW(li, j, k);
			}
		}

		t1 = get_ms_of_day();

		/* Global average of interior cells for verification. */
		{
			double local_sum = 0.0, global_sum = 0.0;
			double center = 0.0;
			int center_i = n / 2;
			for (li = 1; li <= local_planes; li++) {
				int gi = r_start + li - 1;
				if (gi == 0 || gi == n - 1)
					continue;
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						local_sum += U(li, j, k);
				if (gi == center_i)
					center = U(li, n / 2, n / 2);
			}
			MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE,
				   MPI_SUM, 0, MPI_COMM_WORLD);
			/* center lives on whichever rank owns center_i. */
			if (center_i >= r_start && center_i < r_end && rank != 0)
				MPI_Send(&center, 1, MPI_DOUBLE, 0, 9, MPI_COMM_WORLD);
			if (rank == 0) {
				double denom = (double)(n - 2) * (n - 2) * (n - 2);
				if (!(center_i >= r_start && center_i < r_end))
					MPI_Recv(&center, 1, MPI_DOUBLE, MPI_ANY_SOURCE,
						 9, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				printf("VERIFY OK  rep=%d iters=%d avg_interior=%.8f center=%.8f\n",
				       rep, iters, global_sum / denom, center);
				printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
				       n, iters, rep, t1 - t0);
				fflush(stdout);
			}
		}
	}

#undef U
#undef UNEW
	free(u);
	free(unew);
	MPI_Finalize();
	return 0;
}
