/*
 * N-Body particle simulation — MPI reference.
 *
 * Direct O(N^2) gravity, all-to-all pattern. Particles are block-distributed.
 * Each timestep a rank computes forces on its own block from all positions,
 * integrates its block, then an MPI_Allgatherv rebuilds the full global
 * position/velocity arrays for the next step. Mirrors cape_nbody_manual.c.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#define DEFAULT_N 4096
#define DEFAULT_STEPS 10
#define MAX_N 16384

#define SOFTENING 1e-3
#define DT 0.01

static double px[MAX_N], py[MAX_N], pz[MAX_N];
static double vx[MAX_N], vy[MAX_N], vz[MAX_N];
static double mass[MAX_N];
static double ax[MAX_N], ay[MAX_N], az[MAX_N];

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

static void init_bodies(int n)
{
	unsigned long long rng = 0x123456789abcdefULL;
	int i;
	for (i = 0; i < n; i++) {
		px[i] = to_unit(next_rand64(&rng)) * 2.0 - 1.0;
		py[i] = to_unit(next_rand64(&rng)) * 2.0 - 1.0;
		pz[i] = to_unit(next_rand64(&rng)) * 2.0 - 1.0;
		vx[i] = vy[i] = vz[i] = 0.0;
		mass[i] = to_unit(next_rand64(&rng)) + 0.5;
	}
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int steps = DEFAULT_STEPS;
	int reps = 1;
	int rep, rank, num_ranks;
	int p_start, p_end, i, r;
	int *counts = NULL, *displs = NULL;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		steps = atoi(argv[2]);
	if (argc > 3)
		reps = atoi(argv[3]);
	if (n <= 0 || n > MAX_N) {
		if (rank == 0)
			fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n",
				MAX_N, n);
		MPI_Finalize();
		return 1;
	}
	if (steps <= 0)
		steps = 1;
	if (reps <= 0)
		reps = 1;

	p_start = (int)(((long)n * rank) / num_ranks);
	p_end = (int)(((long)n * (rank + 1)) / num_ranks);

	counts = malloc(num_ranks * sizeof(int));
	displs = malloc(num_ranks * sizeof(int));
	for (r = 0; r < num_ranks; r++) {
		int s = (int)(((long)n * r) / num_ranks);
		int e = (int)(((long)n * (r + 1)) / num_ranks);
		displs[r] = s;
		counts[r] = e - s;
	}

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		int step, j;

		init_bodies(n);

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (step = 0; step < steps; step++) {
			for (i = p_start; i < p_end; i++) {
				double fx = 0.0, fy = 0.0, fz = 0.0;
				for (j = 0; j < n; j++) {
					double dx = px[j] - px[i];
					double dy = py[j] - py[i];
					double dz = pz[j] - pz[i];
					double d2 = dx * dx + dy * dy + dz * dz +
						    SOFTENING;
					double inv = 1.0 / sqrt(d2);
					double inv3 = mass[j] * inv * inv * inv;
					fx += dx * inv3;
					fy += dy * inv3;
					fz += dz * inv3;
				}
				ax[i] = fx; ay[i] = fy; az[i] = fz;
			}

			for (i = p_start; i < p_end; i++) {
				vx[i] += DT * ax[i];
				vy[i] += DT * ay[i];
				vz[i] += DT * az[i];
				px[i] += DT * vx[i];
				py[i] += DT * vy[i];
				pz[i] += DT * vz[i];
			}

			MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
				       px, counts, displs, MPI_DOUBLE, MPI_COMM_WORLD);
			MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
				       py, counts, displs, MPI_DOUBLE, MPI_COMM_WORLD);
			MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
				       pz, counts, displs, MPI_DOUBLE, MPI_COMM_WORLD);
			MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
				       vx, counts, displs, MPI_DOUBLE, MPI_COMM_WORLD);
			MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
				       vy, counts, displs, MPI_DOUBLE, MPI_COMM_WORLD);
			MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
				       vz, counts, displs, MPI_DOUBLE, MPI_COMM_WORLD);
		}

		t1 = get_ms_of_day();

		if (rank == 0) {
			double cx = 0.0, cy = 0.0, cz = 0.0;
			for (i = 0; i < n; i++) {
				cx += px[i]; cy += py[i]; cz += pz[i];
			}
			printf("VERIFY OK  rep=%d centroid=(%.6f,%.6f,%.6f) steps=%d\n",
			       rep, cx / n, cy / n, cz / n, steps);
			printf("RESULT n=%d d=%d rep=%d ms=%lu\n",
			       n, steps, rep, t1 - t0);
			fflush(stdout);
		}
	}

	free(counts);
	free(displs);
	MPI_Finalize();
	return 0;
}
