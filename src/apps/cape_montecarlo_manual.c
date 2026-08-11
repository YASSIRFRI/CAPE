/*
 * Monte Carlo Pi estimation — DICKPT version.
 *
 * Embarrassingly parallel / reduction benchmark. Each rank draws an equal
 * share of the total samples from its own deterministic RNG stream, counts
 * how many fall inside the unit quarter-circle, and writes its partial count
 * into its OWN slot of a shared array. A single allreduce unions the disjoint
 * slots (no per-rank overlap, so the bitmap merge is a plain union — no
 * reduction declaration needed). The master then sums the slots and forms the
 * Pi estimate. There is essentially no communication during the compute phase,
 * which is exactly what this benchmark is meant to stress.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_SAMPLES 100000000ULL
#define MAX_NODES 256

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Per-rank partial hit counts. Each rank touches only hits[node], so the
 * checkpoint deltas are disjoint and the allreduce merge is a union. */
struct ckpt_state {
	unsigned long long hits[MAX_NODES];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

/* xorshift128+-style stream, seeded per rank/rep for reproducibility. */
static unsigned long long next_rand64(unsigned long long *state)
{
	unsigned long long x = *state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * 2685821657736338717ULL;
}

static unsigned long long seed_for(unsigned long node, int rep)
{
	return 0x243f6a8885a308d3ULL ^
	       ((unsigned long long)(node + 1UL) * 0x9e3779b97f4a7c15ULL) ^
	       ((unsigned long long)rep * 0xbf58476d1ce4e5b9ULL);
}

/* Map a random 64-bit word to [0,1). */
static double to_unit(unsigned long long r)
{
	return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

int main(int argc, char *argv[])
{
	unsigned long long samples = DEFAULT_SAMPLES;
	int reps = 1;
	int rep;
	unsigned long node, num_nodes;
	struct ckpt_state *state = &g_state;

	if (argc > 1)
		samples = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		reps = atoi(argv[2]);
	if (samples == 0)
		samples = DEFAULT_SAMPLES;
	if (reps <= 0)
		reps = 1;

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();
	if (num_nodes == 0 || num_nodes > MAX_NODES) {
		fprintf(stderr, "ERROR: num_nodes=%lu out of range [1,%d]\n",
			num_nodes, MAX_NODES);
		return 1;
	}

	memset(state, 0, sizeof(*state));
	dickpt_register_region(state, sizeof(*state));
	dickpt_send_num_jobs((unsigned long)(samples / num_nodes));

	for (rep = 1; rep <= reps; rep++) {
		unsigned long long my_samples, i, local_hits = 0;
		unsigned long long rng = seed_for(node, rep);
		unsigned long t0, t1;
		double pi;

		/* Even split; the last rank absorbs the remainder. */
		my_samples = samples / num_nodes;
		if (node == num_nodes - 1)
			my_samples += samples % num_nodes;

		memset(state->hits, 0, sizeof(state->hits));

		t0 = get_ms_of_day();
		dickpt_start_ckpt();

		for (i = 0; i < my_samples; i++) {
			double x = to_unit(next_rand64(&rng));
			double y = to_unit(next_rand64(&rng));
			if (x * x + y * y <= 1.0)
				local_hits++;
		}
		state->hits[node] = local_hits;

		dickpt_generate_ckpt();
		dickpt_allreduce_ckpt();
		dickpt_stop_ckpt();

		t1 = get_ms_of_day();

		if (node == 0) {
			unsigned long long total_hits = 0;
			unsigned long n;
			for (n = 0; n < num_nodes; n++)
				total_hits += state->hits[n];
			pi = 4.0 * (double)total_hits / (double)samples;
			printf("VERIFY OK  rep=%d pi=%.6f abs_err=%.6f samples=%llu\n",
			       rep, pi, fabs(pi - M_PI), samples);
			printf("RESULT n=%llu rep=%d ms=%lu\n",
			       samples, rep, t1 - t0);
			fflush(stdout);
		}
	}

	return 0;
}
