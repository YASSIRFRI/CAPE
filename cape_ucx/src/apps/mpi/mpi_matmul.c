/*
 * Distributed dense matrix multiplication C = A x B — MPI reference.
 *
 * Compute-bound N^3 core. Rows of C are block-distributed; A and B are
 * replicated read-only operands (same seed on every rank). Each rank computes
 * its row stripe and an MPI_Allgatherv assembles the full C. Mirrors
 * cape_matmul_manual.c (double precision).
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#define DEFAULT_N 1024
#define MAX_N 2048

static double a[MAX_N][MAX_N];
static double b[MAX_N][MAX_N];
static double c[MAX_N][MAX_N];

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static unsigned long long next_rand64(unsigned long long *state)
{
	unsigned long long x = *state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * 2685821657736338717ULL;
}

static double to_unit(unsigned long long r)
{
	return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

static int verify_sample(int n, int row_start, int row_end, int rank, int rep)
{
	int checks[4][2];
	int span = (row_end > row_start) ? (row_end - row_start) : 1;
	int t;

	checks[0][0] = row_start;            checks[0][1] = 0;
	checks[1][0] = row_start;            checks[1][1] = n - 1;
	checks[2][0] = row_end - 1;          checks[2][1] = n / 2;
	checks[3][0] = row_start + span / 2; checks[3][1] = n / 3;

	for (t = 0; t < 4; t++) {
		int i = checks[t][0], j = checks[t][1], k;
		double expected = 0.0;
		if (i < 0 || i >= n || j < 0 || j >= n)
			continue;
		for (k = 0; k < n; k++)
			expected += a[i][k] * b[k][j];
		if (fabs(c[i][j] - expected) > 1e-6 * (fabs(expected) + 1.0)) {
			fprintf(stderr,
				"VERIFY FAIL rank=%d rep=%d c[%d][%d] got=%.6f expected=%.6f\n",
				rank, rep, i, j, c[i][j], expected);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep, rank, num_ranks;
	int row_start, row_end, i, j, k, r;
	int *counts = NULL, *displs = NULL;
	unsigned long long rng = 0xdeadbeefcafebabeULL;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		reps = atoi(argv[2]);
	if (n <= 0 || n > MAX_N) {
		if (rank == 0)
			fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n",
				MAX_N, n);
		MPI_Finalize();
		return 1;
	}
	if (reps <= 0)
		reps = 1;

	row_start = (int)(((long)n * rank) / num_ranks);
	row_end = (int)(((long)n * (rank + 1)) / num_ranks);

	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++) {
			a[i][j] = to_unit(next_rand64(&rng));
			b[i][j] = to_unit(next_rand64(&rng));
		}

	/* Allgatherv plan in units of full MAX_N-wide rows. */
	counts = malloc(num_ranks * sizeof(int));
	displs = malloc(num_ranks * sizeof(int));
	for (r = 0; r < num_ranks; r++) {
		int s = (int)(((long)n * r) / num_ranks);
		int e = (int)(((long)n * (r + 1)) / num_ranks);
		displs[r] = s * MAX_N;
		counts[r] = (e - s) * MAX_N;
	}

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;

		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				c[i][j] = 0.0;

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (i = row_start; i < row_end; i++) {
			for (k = 0; k < n; k++) {
				double aik = a[i][k];
				for (j = 0; j < n; j++)
					c[i][j] += aik * b[k][j];
			}
		}

		MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
			       &c[0][0], counts, displs, MPI_DOUBLE,
			       MPI_COMM_WORLD);

		t1 = get_ms_of_day();

		if (verify_sample(n, row_start, row_end, rank, rep) != 0) {
			MPI_Abort(MPI_COMM_WORLD, 1);
			return 1;
		}

		if (rank == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	free(counts);
	free(displs);
	MPI_Finalize();
	return 0;
}
