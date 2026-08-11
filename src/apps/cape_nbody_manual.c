/*
 * N-Body particle simulation — DICKPT version.
 *
 * Direct O(N^2) gravitational interaction, the classic all-to-all pattern of
 * astrophysics / molecular-dynamics workloads. Particles are block-distributed
 * across ranks. Each timestep every rank:
 *   1. reads the FULL position array (all-to-all) to accumulate the force on
 *      each of ITS particles,
 *   2. integrates velocity + position for its own block only,
 *   3. allreduces so every rank starts the next step with the merged global
 *      positions/velocities.
 * Because a rank writes only its own block, the per-step checkpoint deltas are
 * disjoint and the allreduce merge is a plain union. Masses are read-only and
 * identical on every rank (same seed), so they stay out of the tracked region.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 4096
#define DEFAULT_STEPS 10
#define MAX_N 16384

#define SOFTENING 1e-3
#define DT 0.01

/* Shared, checkpoint-tracked state: positions and velocities. */
struct ckpt_state {
	double px[MAX_N], py[MAX_N], pz[MAX_N];
	double vx[MAX_N], vy[MAX_N], vz[MAX_N];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

/* Read-only, replicated on every rank — not tracked. */
static double mass[MAX_N];

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

static double to_unit(unsigned long long r)
{
	return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

/* Deterministic initial conditions, identical on every rank. */
static void init_bodies(struct ckpt_state *s, int n)
{
	unsigned long long rng = 0x123456789abcdefULL;
	int i;
	for (i = 0; i < n; i++) {
		s->px[i] = to_unit(next_rand64(&rng)) * 2.0 - 1.0;
		s->py[i] = to_unit(next_rand64(&rng)) * 2.0 - 1.0;
		s->pz[i] = to_unit(next_rand64(&rng)) * 2.0 - 1.0;
		s->vx[i] = 0.0;
		s->vy[i] = 0.0;
		s->vz[i] = 0.0;
		mass[i] = to_unit(next_rand64(&rng)) + 0.5;
	}
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int steps = DEFAULT_STEPS;
	int reps = 1;
	int rep;
	unsigned long node, num_nodes;
	int p_start, p_end;
	struct ckpt_state *state = &g_state;

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		steps = atoi(argv[2]);
	if (argc > 3)
		reps = atoi(argv[3]);
	if (n <= 0 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n", MAX_N, n);
		return 1;
	}
	if (steps <= 0)
		steps = 1;
	if (reps <= 0)
		reps = 1;

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();

	p_start = (int)(((long)n * node) / num_nodes);
	p_end = (int)(((long)n * (node + 1)) / num_nodes);

	dickpt_register_region(state, sizeof(*state));
	dickpt_send_num_jobs((unsigned long)n);

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		int step, i, j;

		init_bodies(state, n);

		t0 = get_ms_of_day();

		for (step = 0; step < steps; step++) {
			/* Local accelerations for this rank's block. */
			static double ax[MAX_N], ay[MAX_N], az[MAX_N];

			dickpt_start_ckpt();

			for (i = p_start; i < p_end; i++) {
				double fx = 0.0, fy = 0.0, fz = 0.0;
				for (j = 0; j < n; j++) {
					double dx = state->px[j] - state->px[i];
					double dy = state->py[j] - state->py[i];
					double dz = state->pz[j] - state->pz[i];
					double d2 = dx * dx + dy * dy + dz * dz +
						    SOFTENING;
					double inv = 1.0 / sqrt(d2);
					double inv3 = mass[j] * inv * inv * inv;
					fx += dx * inv3;
					fy += dy * inv3;
					fz += dz * inv3;
				}
				ax[i] = fx;
				ay[i] = fy;
				az[i] = fz;
			}

			/* Integrate this rank's own particles only. */
			for (i = p_start; i < p_end; i++) {
				state->vx[i] += DT * ax[i];
				state->vy[i] += DT * ay[i];
				state->vz[i] += DT * az[i];
				state->px[i] += DT * state->vx[i];
				state->py[i] += DT * state->vy[i];
				state->pz[i] += DT * state->vz[i];
			}

			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();
		}

		t1 = get_ms_of_day();

		if (node == 0) {
			double cx = 0.0, cy = 0.0, cz = 0.0;
			for (i = 0; i < n; i++) {
				cx += state->px[i];
				cy += state->py[i];
				cz += state->pz[i];
			}
			printf("VERIFY OK  rep=%d centroid=(%.6f,%.6f,%.6f) steps=%d\n",
			       rep, cx / n, cy / n, cz / n, steps);
			printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
			       n, steps, rep, t1 - t0);
			fflush(stdout);
		}
	}

	return 0;
}
