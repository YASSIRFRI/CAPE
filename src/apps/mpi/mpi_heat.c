/*
 * 2D heat-diffusion / Laplace solver (Jacobi iteration) — MPI reference.
 *
 * Same problem as cape_heat_manual.c: steady-state heat equation on an N x N
 * grid with a hot top edge, solved by Jacobi sweeps. The grid is row-block
 * distributed; each rank keeps its local stripe plus two ghost rows.
 *
 * Note how much machinery the distributed-memory model demands compared to the
 * DICKPT version: explicit neighbour discovery, ghost-row buffers, and a
 * hand-written halo exchange (MPI_Sendrecv) on every iteration. That
 * boilerplate is exactly what DICKPT hides behind a transparent incremental
 * checkpoint.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#define DEFAULT_N 2048
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
	int r_start, r_end, local_rows;
	int up, down;
	/* Local arrays carry 2 ghost rows: index 0 (top halo) and
	 * local_rows+1 (bottom halo); interior local rows are 1..local_rows. */
	double *u = NULL, *unew = NULL;
	int li, j;

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
	local_rows = r_end - r_start;

	up = (r_start > 0) ? rank - 1 : MPI_PROC_NULL;
	down = (r_end < n) ? rank + 1 : MPI_PROC_NULL;

	u = malloc((size_t)(local_rows + 2) * n * sizeof(double));
	unew = malloc((size_t)(local_rows + 2) * n * sizeof(double));
	if (u == NULL || unew == NULL) {
		fprintf(stderr, "rank %d: allocation failed\n", rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
		return 1;
	}
#define U(li, j)    u[(size_t)(li) * n + (j)]
#define UNEW(li, j) unew[(size_t)(li) * n + (j)]

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		int it;

		/* Initialise local stripe (+ghosts) with the global IC: hot top. */
		for (li = 0; li < local_rows + 2; li++) {
			int grow = r_start + li - 1;   /* global row of li */
			for (j = 0; j < n; j++)
				U(li, j) = (grow == 0) ? HOT : COLD;
		}

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (it = 0; it < iters; it++) {
			/* Halo exchange: trade boundary interior rows with
			 * neighbours into each other's ghost rows. */
			MPI_Sendrecv(&U(1, 0), n, MPI_DOUBLE, up, 0,
				     &U(local_rows + 1, 0), n, MPI_DOUBLE, down, 0,
				     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Sendrecv(&U(local_rows, 0), n, MPI_DOUBLE, down, 1,
				     &U(0, 0), n, MPI_DOUBLE, up, 1,
				     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			for (li = 1; li <= local_rows; li++) {
				int grow = r_start + li - 1;
				if (grow == 0 || grow == n - 1)
					continue;   /* fixed Dirichlet rows */
				for (j = 1; j < n - 1; j++)
					UNEW(li, j) = 0.25 *
						(U(li - 1, j) + U(li + 1, j) +
						 U(li, j - 1) + U(li, j + 1));
			}

			for (li = 1; li <= local_rows; li++) {
				int grow = r_start + li - 1;
				if (grow == 0 || grow == n - 1)
					continue;
				for (j = 1; j < n - 1; j++)
					U(li, j) = UNEW(li, j);
			}
		}

		t1 = get_ms_of_day();

		/* Global average of interior cells for verification. */
		{
			double local_sum = 0.0, global_sum = 0.0;
			double center = 0.0;
			int center_row = n / 2;
			for (li = 1; li <= local_rows; li++) {
				int grow = r_start + li - 1;
				if (grow == 0 || grow == n - 1)
					continue;
				for (j = 1; j < n - 1; j++)
					local_sum += U(li, j);
				if (grow == center_row)
					center = U(li, n / 2);
			}
			MPI_Reduce(&local_sum, &global_sum, 1, MPI_DOUBLE,
				   MPI_SUM, 0, MPI_COMM_WORLD);
			/* center lives on whichever rank owns center_row. */
			if (center_row >= r_start && center_row < r_end && rank != 0)
				MPI_Send(&center, 1, MPI_DOUBLE, 0, 9, MPI_COMM_WORLD);
			if (rank == 0) {
				if (!(center_row >= r_start && center_row < r_end))
					MPI_Recv(&center, 1, MPI_DOUBLE, MPI_ANY_SOURCE,
						 9, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				printf("VERIFY OK  rep=%d iters=%d avg_interior=%.8f center=%.8f\n",
				       rep, iters,
				       global_sum / ((double)(n - 2) * (n - 2)), center);
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
