#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_N 4096
#define DEFAULT_D 256
#define MAX_N 8192
#define MAX_D 1024

static int X[MAX_N][MAX_D];
static int w[MAX_D];
static int y[MAX_N];
static long long grad[MAX_D];

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static int verify_full(int n, int d, int rank, int rep)
{
	int errors = 0;
	int i, j, k;
	for (j = 0; j < d; j++) {
		long long expected = 0;
		for (i = 0; i < n; i++) {
			long long r = -(long long)y[i];
			for (k = 0; k < d; k++)
				r += (long long)X[i][k] * w[k];
			expected += (long long)X[i][j] * r;
		}
		if (grad[j] != expected) {
			fprintf(stderr,
				"VERIFY FAIL rank=%d rep=%d grad[%d] got=%lld expected=%lld\n",
				rank, rep, j, grad[j], expected);
			if (++errors >= 10) return errors;
		}
	}
	if (errors == 0 && rank == 0)
		printf("VERIFY OK  rep=%d (grad length %d) all cells correct\n",
		       rep, d);
	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int d = DEFAULT_D;
	int reps = 1;
	int rep, i, j, k;
	unsigned long t0, t1;
	int rank, num_ranks;
	int row_start, row_end;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) d = atoi(argv[2]);
	if (argc > 3) reps = atoi(argv[3]);
	if (n <= 0 || n > MAX_N || d <= 0 || d > MAX_D) {
		if (rank == 0)
			fprintf(stderr,
				"ERROR: need 1<=n<=%d (got %d) and 1<=d<=%d (got %d)\n",
				MAX_N, n, MAX_D, d);
		MPI_Finalize();
		return 1;
	}
	if (reps <= 0) reps = 1;

	row_start = (n * rank) / num_ranks;
	row_end = (n * (rank + 1)) / num_ranks;

	srand(12345);
	for (i = 0; i < n; i++) {
		y[i] = rand() % 10;
		for (j = 0; j < d; j++)
			X[i][j] = rand() % 10;
	}
	for (j = 0; j < d; j++)
		w[j] = rand() % 10;

	for (rep = 1; rep <= reps; rep++) {
		memset(grad, 0, sizeof(grad));

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (i = row_start; i < row_end; i++) {
			long long r = -(long long)y[i];
			for (k = 0; k < d; k++)
				r += (long long)X[i][k] * w[k];
			for (j = 0; j < d; j++)
				grad[j] += (long long)X[i][j] * r;
		}

		MPI_Allreduce(MPI_IN_PLACE, grad, MAX_D, MPI_LONG_LONG,
			      MPI_SUM, MPI_COMM_WORLD);

		t1 = get_ms_of_day();

		if (verify_full(n, d, rank, rep) != 0) {
			MPI_Abort(MPI_COMM_WORLD, 1);
			return 1;
		}
		if (rank == 0)
			printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
			       n, d, rep, t1 - t0);
	}

	MPI_Finalize();
	return 0;
}
