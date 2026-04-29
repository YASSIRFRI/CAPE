/*
 * write_stress (pure MPI version):
 *   Per-rank row in a 2D state, mirroring the CAPE/DICKPT layout.
 *   Each phase: zero the full state, write scattered random values
 *   into own row, MPI_Allreduce(BXOR) merges every rank's row into
 *   all ranks' state.
 */
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <limits.h>

#define DEFAULT_N       (1U << 20)
#define MAX_N           (1U << 20)
#define DEFAULT_PHASES  8
#define MAX_PHASES      32
#define WRITES_PER_CELL 4U

static unsigned long get_ms_of_day(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static uint64_t next_rand64(uint64_t *state)
{
	uint64_t x = *state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * 2685821657736338717ULL;
}

static uint64_t seed_for(unsigned int rank, int rep, int phase)
{
	return 0x9e3779b97f4a7c15ULL ^
	       ((uint64_t)(rank + 1U) * 0xbf58476d1ce4e5b9ULL) ^
	       ((uint64_t)rep   * 0x94d049bb133111ebULL) ^
	       ((uint64_t)(phase + 1) * 0xd1b54a32d192ed03ULL);
}

static unsigned int run_phase(unsigned int *row, int n,
			      unsigned int rank, int rep, int phase,
			      int do_writes)
{
	uint64_t rng = seed_for(rank, rep, phase);
	size_t writes = (size_t)n * WRITES_PER_CELL;
	unsigned int expected_xor = 0;
	size_t t;

	for (t = 0; t < writes; t++) {
		uint64_t r = next_rand64(&rng);
		size_t idx = (size_t)(r % (uint64_t)n);
		unsigned int value =
			(unsigned int)(next_rand64(&rng) ^ (r >> 32) ^ t);

		if (do_writes)
			row[idx] ^= value;
		expected_xor ^= value;
	}

	return expected_xor;
}

static unsigned int checksum_row(const unsigned int *row, int n)
{
	unsigned int x = 0;
	int i;

	for (i = 0; i < n; i++)
		x ^= row[i];

	return x;
}

static int verify(const unsigned int *rows, int n, int num_ranks,
		  int rank, int rep, int phase)
{
	int errors = 0;
	int r;

	for (r = 0; r < num_ranks; r++) {
		const unsigned int *row = rows + ((size_t)r * (size_t)n);
		unsigned int got = checksum_row(row, n);
		unsigned int expected =
			run_phase(NULL, n, (unsigned int)r, rep, phase, 0);

		if (got != expected) {
			fprintf(stderr,
				"VERIFY FAIL rank=%d rep=%d row=%d phase=%d got=0x%08x expected=0x%08x\n",
				rank, rep, r, phase, got, expected);
			if (++errors >= 10)
				return errors;
		}
	}

	if (errors == 0 && rank == 0)
		printf("VERIFY OK  rep=%d phase=%d (%d rows x %d cells, %u writes/cell/phase)\n",
		       rep, phase, num_ranks, n, WRITES_PER_CELL);
	return errors;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int phases = DEFAULT_PHASES;
	int reps = 1;
	int rep, p;
	int rank, num_ranks;
	unsigned long t0, t1;
	unsigned int *rows;
	size_t total_cells;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (argc > 1) n = atoi(argv[1]);
	if (argc > 2) phases = atoi(argv[2]);
	if (argc > 3) reps = atoi(argv[3]);
	if (n <= 0 || n > (int)MAX_N) {
		if (rank == 0)
			fprintf(stderr, "ERROR: n must be in [1, %u], got %d\n",
				MAX_N, n);
		MPI_Finalize();
		return 1;
	}
	if (phases <= 0 || phases > MAX_PHASES) {
		if (rank == 0)
			fprintf(stderr, "ERROR: phases must be in [1, %d], got %d\n",
				MAX_PHASES, phases);
		MPI_Finalize();
		return 1;
	}
	if (reps <= 0) reps = 1;

	total_cells = (size_t)num_ranks * (size_t)n;
	if (total_cells > (size_t)INT_MAX) {
		if (rank == 0)
			fprintf(stderr,
				"ERROR: total cells too large: %zu\n",
				total_cells);
		MPI_Finalize();
		return 1;
	}

	rows = calloc(total_cells, sizeof(*rows));
	if (!rows) {
		fprintf(stderr, "rank %d: failed to allocate %zu cells\n",
			rank, total_cells);
		MPI_Abort(MPI_COMM_WORLD, 1);
		return 1;
	}

	for (rep = 1; rep <= reps; rep++) {
		unsigned int *my_row = rows + ((size_t)rank * (size_t)n);

		MPI_Barrier(MPI_COMM_WORLD);
		t0 = get_ms_of_day();

		for (p = 0; p < phases; p++) {
			memset(rows, 0, total_cells * sizeof(*rows));
			run_phase(my_row, n,
				  (unsigned int)rank, rep, p, 1);
			MPI_Allreduce(MPI_IN_PLACE, rows, (int)total_cells,
				      MPI_UNSIGNED, MPI_BXOR, MPI_COMM_WORLD);
		}

		t1 = get_ms_of_day();

		if (verify(rows, n, num_ranks, rank, rep, phases - 1) != 0) {
			free(rows);
			MPI_Abort(MPI_COMM_WORLD, 1);
			return 1;
		}
		if (rank == 0)
			printf("RESULT n=%d phases=%d rep=%d ms=%lu\n",
			       n, phases, rep, t1 - t0);
	}

	free(rows);
	MPI_Finalize();
	return 0;
}
