#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_N 1024
#define MAX_N 3200

static int a[MAX_N][MAX_N];
static int b[MAX_N][MAX_N];
static int c[MAX_N][MAX_N];

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static int verify_full(int n, int row_start, int row_end, int rank, int rep)
{
	int errors = 0;
	int i, j, k;

	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			long long expected = 0;
			for (k = 0; k < n; k++)
				expected += (long long)a[i][k] * b[k][j];

			if ((long long)c[i][j] != expected) {
				const char *origin = (i >= row_start && i < row_end)
						     ? "LOCAL" : "REMOTE";
				fprintf(stderr,
					"VERIFY FAIL [%s] rank=%d rep=%d "
					"c[%d][%d] got=%d expected=%lld\n",
					origin, rank, rep, i, j,
					c[i][j], expected);
				if (++errors >= 10) {
					fprintf(stderr,
						"... too many errors, stopping\n");
					return errors;
				}
			}
		}
	}

	if (errors == 0 && rank == 0)
		printf("VERIFY OK  rep=%d (%d x %d) all %d cells correct\n",
		       rep, n, n, n * n);

	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep;
	unsigned long t0, t1;
	int rank, num_ranks;
	int i, j, k, sum;
	int row_start, row_end;

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

	row_start = (n * rank) / num_ranks;
	row_end = (n * (rank + 1)) / num_ranks;

	srand(12345);
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			a[i][j] = rand() % 100;
			b[i][j] = rand() % 100;
			c[i][j] = 0;
		}
	}

	for (rep = 1; rep <= reps; rep++) {
		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				c[i][j] = 0;

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (i = row_start; i < row_end; i++) {
			for (j = 0; j < n; j++) {
				sum = 0;
				for (k = 0; k < n; k++)
					sum += a[i][k] * b[k][j];
				c[i][j] = sum;
			}
		}

		MPI_Allreduce(MPI_IN_PLACE, &c[0][0], n * MAX_N,
			      MPI_INT, MPI_SUM, MPI_COMM_WORLD);

		t1 = get_ms_of_day();

		if (verify_full(n, row_start, row_end, rank, rep) != 0) {
			MPI_Abort(MPI_COMM_WORLD, 1);
			return 1;
		}

		if (rank == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	MPI_Finalize();
	return 0;
}
