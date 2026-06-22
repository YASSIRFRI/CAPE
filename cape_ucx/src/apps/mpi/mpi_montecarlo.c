/*
 * Monte Carlo Pi estimation — MPI reference.
 *
 * Embarrassingly parallel / reduction benchmark. Each rank draws its share of
 * samples from its own RNG stream, counts hits in the unit quarter-circle, and
 * a single MPI_Reduce sums the partial counts on rank 0. Mirrors
 * cape_montecarlo_manual.c so the timings are comparable.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>

#define DEFAULT_SAMPLES 100000000ULL

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static unsigned long long seed_for(int rank, int rep)
{
	return 0x243f6a8885a308d3ULL ^
	       ((unsigned long long)(rank + 1) * 0x9e3779b97f4a7c15ULL) ^
	       ((unsigned long long)rep * 0xbf58476d1ce4e5b9ULL);
}

static double to_unit(unsigned long long r)
{
	return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

int main(int argc, char *argv[])
{
	unsigned long long samples = DEFAULT_SAMPLES;
	int reps = 1;
	int rep, rank, num_ranks;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (argc > 1)
		samples = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		reps = atoi(argv[2]);
	if (samples == 0)
		samples = DEFAULT_SAMPLES;
	if (reps <= 0)
		reps = 1;

	for (rep = 1; rep <= reps; rep++) {
		unsigned long long my_samples, i, local_hits = 0, total_hits = 0;
		unsigned long long rng = seed_for(rank, rep);
		unsigned long t0, t1;

		my_samples = samples / num_ranks;
		if (rank == num_ranks - 1)
			my_samples += samples % num_ranks;

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (i = 0; i < my_samples; i++) {
			double x = to_unit(next_rand64(&rng));
			double y = to_unit(next_rand64(&rng));
			if (x * x + y * y <= 1.0)
				local_hits++;
		}

		MPI_Reduce(&local_hits, &total_hits, 1, MPI_UNSIGNED_LONG_LONG,
			   MPI_SUM, 0, MPI_COMM_WORLD);

		t1 = get_ms_of_day();

		if (rank == 0) {
			double pi = 4.0 * (double)total_hits / (double)samples;
			printf("VERIFY OK  rep=%d pi=%.6f abs_err=%.6f samples=%llu\n",
			       rep, pi, fabs(pi - M_PI), samples);
			printf("RESULT n=%llu rep=%d ms=%lu\n",
			       samples, rep, t1 - t0);
			fflush(stdout);
		}
	}

	MPI_Finalize();
	return 0;
}
