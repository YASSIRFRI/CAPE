#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_N 2048
#define MAX_N 4096

static int A[MAX_N][MAX_N];
static int x[MAX_N];
static int y[MAX_N];

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static int verify_full(int n, int rank, int rep)
{
	int errors = 0;
	int i, k;
	for (i = 0; i < n; i++) {
		long long expected = 0;
		for (k = 0; k < n; k++)
			expected += (long long)A[i][k] * x[k];
		if ((long long)y[i] != expected) {
			fprintf(stderr,
				"VERIFY FAIL rank=%d rep=%d y[%d] got=%d expected=%lld\n",
				rank, rep, i, y[i], expected);
			if (++errors >= 10) return errors;
		}
	}
	if (errors == 0 && rank == 0)
		printf("VERIFY OK  rep=%d (y length %d) all cells correct\n", rep, n);
	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep, i, k;
	unsigned long t0, t1;
	int rank, num_ranks;
	int row_start, row_end;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) reps = atoi(argv[2]);
	if (n <= 0 || n > MAX_N) {
		if (rank == 0)
			fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n",
				MAX_N, n);
		MPI_Finalize();
		return 1;
	}
	if (reps <= 0) reps = 1;

	row_start = (n * rank) / num_ranks;
	row_end = (n * (rank + 1)) / num_ranks;

	srand(12345);
	for (i = 0; i < n; i++) {
		x[i] = rand() % 100;
		for (k = 0; k < n; k++)
			A[i][k] = rand() % 100;
	}

	for (rep = 1; rep <= reps; rep++) {
		memset(y, 0, sizeof(y));

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (i = row_start; i < row_end; i++) {
			long long sum = 0;
			for (k = 0; k < n; k++)
				sum += (long long)A[i][k] * x[k];
			y[i] = (int)sum;
		}

		MPI_Allreduce(MPI_IN_PLACE, y, MAX_N, MPI_INT,
			      MPI_SUM, MPI_COMM_WORLD);

		t1 = get_ms_of_day();

		if (verify_full(n, rank, rep) != 0) {
			MPI_Abort(MPI_COMM_WORLD, 1);
			return 1;
		}
		if (rank == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	MPI_Finalize();
	return 0;
}
