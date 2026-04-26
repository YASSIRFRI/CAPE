#include <omp.h>
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

static void generate(int n, int d)
{
	int i, j;

	srand(12345);
	for (i = 0; i < n; i++) {
		y[i] = rand() % 10;
		for (j = 0; j < d; j++)
			X[i][j] = rand() % 10;
	}
	for (j = 0; j < d; j++)
		w[j] = rand() % 10;
}

static void compute_gradient(int n, int d)
{
	int i, j, k;

	memset(grad, 0, sizeof(grad));

#pragma omp parallel private(i, j, k)
	{
		long long local_grad[MAX_D];

		memset(local_grad, 0, sizeof(local_grad));

#pragma omp for schedule(static)
		for (i = 0; i < n; i++) {
			long long r = -(long long)y[i];

			for (k = 0; k < d; k++)
				r += (long long)X[i][k] * w[k];
			for (j = 0; j < d; j++)
				local_grad[j] += (long long)X[i][j] * r;
		}

#pragma omp critical
		{
			for (j = 0; j < d; j++)
				grad[j] += local_grad[j];
		}
	}
}

static int verify_full(int n, int d, int rep)
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
				"VERIFY FAIL rep=%d grad[%d] got=%lld expected=%lld\n",
				rep, j, grad[j], expected);
			if (++errors >= 10)
				return errors;
		}
	}

	if (errors == 0)
		printf("VERIFY OK  rep=%d (grad length %d) all cells correct\n",
		       rep, d);

	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int d = DEFAULT_D;
	int reps = 1;
	int rep;

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		d = atoi(argv[2]);
	if (argc > 3)
		reps = atoi(argv[3]);
	if (n <= 0 || n > MAX_N || d <= 0 || d > MAX_D) {
		fprintf(stderr,
			"ERROR: need 1<=n<=%d (got %d) and 1<=d<=%d (got %d)\n",
			MAX_N, n, MAX_D, d);
		return 1;
	}
	if (reps <= 0)
		reps = 1;

	generate(n, d);

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;

		t0 = get_ms_of_day();
		compute_gradient(n, d);
		t1 = get_ms_of_day();

		if (verify_full(n, d, rep) != 0)
			return 1;

		printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
		       n, d, rep, t1 - t0);
	}

	return 0;
}
