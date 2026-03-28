#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "../../include/cape.h"

#define DEFAULT_N 100
#define MAX_N 12000

unsigned long get_ms_of_day()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

void *__get_pc()
{
	return __builtin_return_address(0);
}

int a[MAX_N][MAX_N];
int b[MAX_N][MAX_N];
int c[MAX_N][MAX_N];

void generate_matrix(int n)
{
	__enter_func();
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	int i, j;
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&j, CAPE_INT, 1, 0);
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			a[i][j] = rand() % 100;
			b[i][j] = rand() % 100;
			c[i][j] = 0;
		}
	}
	__exit_func();
}

void matrix_mult(int n)
{
	__enter_func();
	int i, j, k;
	cape_begin(PARALLEL_FOR, 0, n);
	cape_set_private(&a);
	cape_set_private(&b);
	ckpt_start();
	for (i = __left__; i < __right__; i++) {
		for (j = 0; j < n; j++) {
			int sum = 0;
			for (k = 0; k < n; k++)
				sum += a[i][k] * b[k][j];
			c[i][j] = sum;
		}
	}
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL_FOR, FALSE);
	__exit_func();
}

int main(int argc, char **argv)
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep;
	unsigned long t0, t1;

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		reps = atoi(argv[2]);
	if (n <= 0 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n", MAX_N, n);
		return 1;
	}
	if (reps <= 0)
		reps = 1;

	cape_declare_variable(&a, CAPE_INT, MAX_N * MAX_N, 0);
	cape_declare_variable(&b, CAPE_INT, MAX_N * MAX_N, 0);
	cape_declare_variable(&c, CAPE_INT, MAX_N * MAX_N, 0);

	cape_init();

	cape_declare_variable(&n, CAPE_INT, 1, 0);
	srand(12345);
	generate_matrix(n);

	for (rep = 1; rep <= reps; rep++) {
		t0 = get_ms_of_day();
		matrix_mult(n);
		t1 = get_ms_of_day();
		if (cape_get_node_num() == 0)
			printf("RESULT n=%d rep=%d ms=%lu\n", n, rep, t1 - t0);
	}

	cape_finalize();
	return 0;
}
