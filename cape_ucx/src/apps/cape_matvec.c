#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "../../include/cape.h"

#define DEFAULT_N 2048
#define MAX_N 4096

unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

void *__get_pc(void) { return __builtin_return_address(0); }

int A[MAX_N][MAX_N];
int x[MAX_N];
int y[MAX_N];

void generate(int n)
{
	__enter_func();
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	int i, k;
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&k, CAPE_INT, 1, 0);
	for (i = 0; i < n; i++) {
		x[i] = rand() % 100;
		y[i] = 0;
		for (k = 0; k < n; k++)
			A[i][k] = rand() % 100;
	}
	__exit_func();
}

void matvec(int n)
{
	__enter_func();
	int i, k;
	cape_begin(PARALLEL_FOR, 0, n);
	cape_set_private(&A);
	cape_set_private(&x);
	ckpt_start();
	for (i = __left__; i < __right__; i++) {
		long long sum = 0;
		for (k = 0; k < n; k++)
			sum += (long long)A[i][k] * x[k];
		y[i] = (int)sum;
	}
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL_FOR, FALSE);
	__exit_func();
}

static int verify_full(int n, unsigned long node, int rep)
{
	int errors = 0;
	int i, k;
	for (i = 0; i < n; i++) {
		long long expected = 0;
		for (k = 0; k < n; k++)
			expected += (long long)A[i][k] * x[k];
		if ((long long)y[i] != expected) {
			fprintf(stderr,
				"VERIFY FAIL node=%lu rep=%d y[%d] got=%d expected=%lld\n",
				node, rep, i, y[i], expected);
			if (++errors >= 10) return errors;
		}
	}
	if (errors == 0 && node == 0)
		printf("VERIFY OK  rep=%d (y length %d) all cells correct\n", rep, n);
	return errors;
}

int main(int argc, char **argv)
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep;
	unsigned long t0, t1;

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) reps = atoi(argv[2]);
	if (n <= 0 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n", MAX_N, n);
		return 1;
	}
	if (reps <= 0) reps = 1;

	cape_declare_variable(&A, CAPE_INT, MAX_N * MAX_N, 0);
	cape_declare_variable(&x, CAPE_INT, MAX_N, 0);
	cape_declare_variable(&y, CAPE_INT, MAX_N, 0);

	cape_init();

	cape_declare_variable(&n, CAPE_INT, 1, 0);
	srand(12345);
	generate(n);

	for (rep = 1; rep <= reps; rep++) {
		t0 = get_ms_of_day();
		matvec(n);
		t1 = get_ms_of_day();
		if (verify_full(n, cape_get_node_num(), rep) != 0) {
			cape_finalize();
			return 1;
		}
		if (cape_get_node_num() == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	cape_finalize();
	return 0;
}
