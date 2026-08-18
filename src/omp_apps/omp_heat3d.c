/*
 * 3D diffusion solver (Jacobi 7-point stencil) — plain OpenMP reference.
 *
 * Shared-memory baseline for the DICKPT/CAPE comparison. Same physics, same
 * grid, same conditional write-back, same RESULT line as
 * src/apps/cape_heat3d_manual.c — the ONLY difference is that there is no
 * checkpointing at all: one process, one cube, threads write it directly.
 *
 * That is exactly the point of the comparison: CAPE gives the programmer this
 * OpenMP programming model but executes it across nodes, paying an incremental
 * checkpoint (dirty-page capture + word diff + union allreduce) per iteration.
 * Running the identical kernel here with pure OpenMP isolates that overhead:
 *
 *   omp   app_ms = sweep_ms + writeback_ms                      (compute only)
 *   cape  app_ms = sweep_ms + writeback_ms + ckpt_ms            (+ DICKPT)
 *
 * The whole cube is updated here (planes 1..n-2), i.e. the work one CAPE rank
 * does times the number of ranks. So compare per-iteration compute rates, and
 * read cape's ckpt_ms as the price of distribution.
 *
 * Loop structure is kept identical to the DICKPT app on purpose:
 *   phase 1 (sweep):     unew = stencil(u)      — read u, write scratch
 *   phase 2 (writeback): if (unew != u) u = unew — the conditional store that
 *                        keeps the dirty set (and hence CAPE's checkpoint)
 *                        small ahead of the diffusion front. Kept here too so
 *                        both versions execute the same instruction mix.
 * The two phases must stay separate: the write-back mutates planes that a
 * neighbouring thread's sweep reads.
 *
 * PAPI: built with PAPI=1, per-phase hardware counters (loads/stores, L1/L2/L3
 * misses, TLB misses, cycles) are accumulated over all threads and dumped at
 * exit — the baseline against which the monitor's checkpoint-path counters are
 * compared.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <omp.h>
#include "../../include/cape_papi.h"

#define DEFAULT_N 128
#define DEFAULT_ITERS 100
#define MAX_N 512

#define HOT 1.0
#define COLD 0.0

static struct cape_papi_region papi_sweep = CAPE_PAPI_REGION("omp_sweep");
static struct cape_papi_region papi_writeback = CAPE_PAPI_REGION("omp_writeback");

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static unsigned long get_us_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000000UL + tv.tv_usec);
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int iters = DEFAULT_ITERS;
	int reps = 1;
	int rep, i, j, k;
	size_t plane, bytes;
	double *u = NULL, *unew = NULL;
	int nthreads;

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		iters = atoi(argv[2]);
	if (argc > 3)
		reps = atoi(argv[3]);
	if (n <= 2 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [3, %d], got %d\n", MAX_N, n);
		return 1;
	}
	if (iters <= 0)
		iters = 1;
	if (reps <= 0)
		reps = 1;

	/* CAPE_COMPUTE_THREADS is honoured so the same sweep driver can set the
	 * thread count for both implementations; OMP_NUM_THREADS still wins if
	 * CAPE_COMPUTE_THREADS is unset. */
	{
		const char *e = getenv("CAPE_COMPUTE_THREADS");
		if (e != NULL && e[0] != '\0' && atoi(e) > 0)
			omp_set_num_threads(atoi(e));
	}

	plane = (size_t)n * (size_t)n;
	bytes = plane * (size_t)n * sizeof(double);
	u = (double *)malloc(bytes);
	unew = (double *)malloc(bytes);
	if (u == NULL || unew == NULL) {
		fprintf(stderr, "allocation failed (%zu bytes)\n", bytes);
		return 1;
	}

	cape_papi_init();

	nthreads = omp_get_max_threads();
	printf("heat3d-omp: %d thread(s), n=%d iters=%d reps=%d "
	       "cube=%.1f MiB\n", nthreads, n, iters, reps,
	       (double)bytes / (1024.0 * 1024.0));

#define U(i, j, k)    u[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]
#define UNEW(i, j, k) unew[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1, ts;
		unsigned long sweep_us = 0, writeback_us = 0;
		int it;

		/* Same initial/boundary condition as the DICKPT version: cold
		 * cube, a small HOT square patch at the centre of the i==0 face
		 * held Dirichlet, so the perturbation is local and spreads as a
		 * growing hemisphere. */
		{
			int cj = n / 2, ck = n / 2;
			int hot_r = n / 32;
			if (hot_r < 1)
				hot_r = 1;
			for (i = 0; i < n; i++)
				for (j = 0; j < n; j++)
					for (k = 0; k < n; k++)
						U(i, j, k) = COLD;
			for (j = 0; j < n; j++)
				for (k = 0; k < n; k++)
					U(0, j, k) = (abs(j - cj) <= hot_r &&
						      abs(k - ck) <= hot_r)
							     ? HOT : COLD;
		}

		t0 = get_ms_of_day();

		for (it = 0; it < iters; it++) {
			ts = get_us_of_day();
#pragma omp parallel
			{
				struct cape_papi_probe pr;
				cape_papi_start(&papi_sweep, &pr);
#pragma omp for schedule(static) private(j, k)
				for (i = 1; i < n - 1; i++)
					for (j = 1; j < n - 1; j++)
						for (k = 1; k < n - 1; k++)
							UNEW(i, j, k) = (1.0 / 6.0) *
								(U(i - 1, j, k) + U(i + 1, j, k) +
								 U(i, j - 1, k) + U(i, j + 1, k) +
								 U(i, j, k - 1) + U(i, j, k + 1));
				cape_papi_stop(&papi_sweep, &pr);
			}
			sweep_us += get_us_of_day() - ts;

			ts = get_us_of_day();
#pragma omp parallel
			{
				struct cape_papi_probe pr;
				cape_papi_start(&papi_writeback, &pr);
#pragma omp for schedule(static) private(j, k)
				for (i = 1; i < n - 1; i++)
					for (j = 1; j < n - 1; j++)
						for (k = 1; k < n - 1; k++)
							if (UNEW(i, j, k) != U(i, j, k))
								U(i, j, k) = UNEW(i, j, k);
				cape_papi_stop(&papi_writeback, &pr);
			}
			writeback_us += get_us_of_day() - ts;
		}

		t1 = get_ms_of_day();

		{
			double sum = 0.0;
			double denom = (double)(n - 2) * (n - 2) * (n - 2);
			for (i = 1; i < n - 1; i++)
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						sum += U(i, j, k);
			printf("VERIFY OK  rep=%d iters=%d avg_interior=%.8f center=%.8f\n",
			       rep, iters, sum / denom, U(n / 2, n / 2, n / 2));
			/* Same key=value shape as the DICKPT RESULT line so one
			 * awk parser handles both; the checkpoint fields are
			 * structurally zero here (no checkpointing exists). */
			printf("RESULT n=%d d=%d rep=%d ms=%lu "
			       "sweep_ms=%lu writeback_ms=%lu ckpt_ms=0 "
			       "start_ms=0 gen_ms=0 allred_ms=0 stop_ms=0 "
			       "threads=%d\n",
			       n, iters, rep, t1 - t0,
			       sweep_us / 1000UL, writeback_us / 1000UL,
			       nthreads);
			fflush(stdout);
		}
	}

#undef U
#undef UNEW
	cape_papi_report();
	free(u);
	free(unew);
	return 0;
}
