#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "../../include/cape.h"

/* Distributed gradient of MSE loss.
 * Each rank writes its partial gradient into its own page-aligned row of
 * grad_parts[MAX_NODES][MAX_D]. After cape_end merges the disjoint pages,
 * every rank sums the rows locally to produce the full gradient. */

#define DEFAULT_N 4096
#define DEFAULT_D 256
#define MAX_N 8192
#define MAX_D 1024           /* keep rows page-aligned to avoid false-sharing */
#define MAX_NODES 32

unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

void *__get_pc(void) { return __builtin_return_address(0); }

int X[MAX_N][MAX_D];
int w[MAX_D];
int y[MAX_N];
long long grad_parts[MAX_NODES][MAX_D] __attribute__((aligned(4096)));
long long grad[MAX_D];

void generate(int n, int d)
{
	__enter_func();
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	cape_declare_variable(&d, CAPE_INT, 1, 0);
	int i, j;
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&j, CAPE_INT, 1, 0);
	for (i = 0; i < n; i++) {
		y[i] = rand() % 10;
		for (j = 0; j < d; j++)
			X[i][j] = rand() % 10;
	}
	for (j = 0; j < d; j++)
		w[j] = rand() % 10;
	for (i = 0; i < MAX_NODES; i++)
		for (j = 0; j < MAX_D; j++)
			grad_parts[i][j] = 0;
	__exit_func();
}

void compute(int n, int d)
{
	__enter_func();
	int i, j, k, node;
	cape_begin(PARALLEL_FOR, 0, n);
	cape_set_private(&X);
	cape_set_private(&w);
	cape_set_private(&y);
	ckpt_start();
	node = cape_get_node_num();
	for (i = __left__; i < __right__; i++) {
		long long r = -(long long)y[i];
		for (k = 0; k < d; k++)
			r += (long long)X[i][k] * w[k];
		for (j = 0; j < d; j++)
			grad_parts[node][j] += (long long)X[i][j] * r;
	}
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL_FOR, FALSE);
	__exit_func();
}

static int verify_full(int n, int d, unsigned long node, int rep)
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
				"VERIFY FAIL node=%lu rep=%d grad[%d] got=%lld expected=%lld\n",
				node, rep, j, grad[j], expected);
			if (++errors >= 10) return errors;
		}
	}
	if (errors == 0 && node == 0)
		printf("VERIFY OK  rep=%d (grad length %d) all cells correct\n",
		       rep, d);
	return errors;
}

int main(int argc, char **argv)
{
	int n = DEFAULT_N;
	int d = DEFAULT_D;
	int reps = 1;
	int rep, i, j;
	unsigned long t0, t1;

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) d = atoi(argv[2]);
	if (argc > 3) reps = atoi(argv[3]);
	if (n <= 0 || n > MAX_N || d <= 0 || d > MAX_D) {
		fprintf(stderr,
			"ERROR: need 1<=n<=%d (got %d) and 1<=d<=%d (got %d)\n",
			MAX_N, n, MAX_D, d);
		return 1;
	}
	if (reps <= 0) reps = 1;

	cape_declare_variable(&X, CAPE_INT, MAX_N * MAX_D, 0);
	cape_declare_variable(&w, CAPE_INT, MAX_D, 0);
	cape_declare_variable(&y, CAPE_INT, MAX_N, 0);
	cape_declare_variable(&grad_parts, CAPE_LONG,
			     MAX_NODES * MAX_D * 2, 0); /* long long = 2 longs */
	cape_declare_variable(&grad, CAPE_LONG, MAX_D * 2, 0);

	cape_init();

	cape_declare_variable(&n, CAPE_INT, 1, 0);
	cape_declare_variable(&d, CAPE_INT, 1, 0);
	srand(12345);
	generate(n, d);

	for (rep = 1; rep <= reps; rep++) {
		for (i = 0; i < MAX_NODES; i++)
			for (j = 0; j < d; j++)
				grad_parts[i][j] = 0;

		t0 = get_ms_of_day();
		compute(n, d);

		for (j = 0; j < d; j++) {
			long long s = 0;
			for (i = 0; i < MAX_NODES; i++)
				s += grad_parts[i][j];
			grad[j] = s;
		}
		t1 = get_ms_of_day();

		if (verify_full(n, d, cape_get_node_num(), rep) != 0) {
			cape_finalize();
			return 1;
		}
		if (cape_get_node_num() == 0)
			printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
			       n, d, rep, t1 - t0);
	}

	cape_finalize();
	return 0;
}
