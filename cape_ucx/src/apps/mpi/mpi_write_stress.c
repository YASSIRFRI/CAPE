/*
 * write_stress (pure MPI version):
 *   Per-rank row in a 2D state, mirroring the CAPE/DICKPT layout.
 *   Each phase: zero the full state, write scattered random values
 *   into own row, then exchange rows with MPI_Sendrecv so every rank
 *   ends with the full state without a collective row merge.
 */
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define DEFAULT_N       (1U << 20)
#define MAX_N           (1U << 20)
#define DEFAULT_PHASES  8
#define MAX_PHASES      32
#define WRITES_PER_CELL 4U

static int profile_enabled(void)
{
	const char *v = getenv("MPI_WRITE_STRESS_PROFILE");

	if (!v || !v[0])
		return 1;
	if (strcmp(v, "0") == 0 ||
	    strcmp(v, "false") == 0 ||
	    strcmp(v, "no") == 0 ||
	    strcmp(v, "off") == 0)
		return 0;
	return 1;
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

static void exchange_rows_sendrecv(unsigned int *rows, int n,
				   int rank, int num_ranks, int phase)
{
	int left = (rank - 1 + num_ranks) % num_ranks;
	int right = (rank + 1) % num_ranks;
	int step;

	for (step = 0; step < num_ranks - 1; step++) {
		int send_owner = (rank - step + num_ranks) % num_ranks;
		int recv_owner = (rank - step - 1 + num_ranks) % num_ranks;
		unsigned int *send_row =
			rows + ((size_t)send_owner * (size_t)n);
		unsigned int *recv_row =
			rows + ((size_t)recv_owner * (size_t)n);

		MPI_Sendrecv(send_row, n, MPI_UNSIGNED, right, phase,
			     recv_row, n, MPI_UNSIGNED, left, phase,
			     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
	}
}

static double sum_phases(const double *values, int phases)
{
	double total = 0.0;
	int p;

	for (p = 0; p < phases; p++)
		total += values[p];
	return total;
}

static void reduce_profile_metric(double local, double *out, int rank)
{
	double reduced[3];

	MPI_Reduce(&local, &reduced[0], 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
	MPI_Reduce(&local, &reduced[1], 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	MPI_Reduce(&local, &reduced[2], 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
	if (rank == 0 && out) {
		out[0] = reduced[0];
		out[1] = reduced[1];
		out[2] = reduced[2];
	}
}

static void reduce_profile_array(const double *local,
				 double *mins, double *sums, double *maxs,
				 int phases)
{
	MPI_Reduce(local, mins,  phases, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
	MPI_Reduce(local, sums,  phases, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	MPI_Reduce(local, maxs,  phases, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
}

static void print_metric(const char *label, const double *m, int num_ranks)
{
	fprintf(stderr, "  %-18s : min=%10.3f  avg=%10.3f  max=%10.3f\n",
		label, m[0] * 1000.0, (m[1] / (double)num_ranks) * 1000.0,
		m[2] * 1000.0);
}

static void profile_report(int rank, int num_ranks, int n, int phases, int rep,
			   double barrier_s, double total_s, double verify_s,
			   const double *phase_zero_s,
			   const double *phase_work_s,
			   const double *phase_sendrecv_s,
			   const double *phase_total_s)
{
	double barrier_m[3], total_m[3], zero_m[3], work_m[3];
	double sendrecv_m[3], phase_total_m[3], verify_m[3];
	double zero_min[MAX_PHASES], zero_sum[MAX_PHASES], zero_max[MAX_PHASES];
	double work_min[MAX_PHASES], work_sum[MAX_PHASES], work_max[MAX_PHASES];
	double comm_min[MAX_PHASES], comm_sum[MAX_PHASES], comm_max[MAX_PHASES];
	double phase_min[MAX_PHASES], phase_sum[MAX_PHASES], phase_max[MAX_PHASES];
	double zero_total = sum_phases(phase_zero_s, phases);
	double work_total = sum_phases(phase_work_s, phases);
	double sendrecv_total = sum_phases(phase_sendrecv_s, phases);
	double phase_total = sum_phases(phase_total_s, phases);
	size_t bytes_per_step = (size_t)n * sizeof(unsigned int);
	size_t bytes_per_phase = bytes_per_step * (size_t)(num_ranks - 1);
	size_t bytes_per_rank_total = bytes_per_phase * (size_t)phases;
	int p;

	reduce_profile_metric(barrier_s, barrier_m, rank);
	reduce_profile_metric(total_s, total_m, rank);
	reduce_profile_metric(zero_total, zero_m, rank);
	reduce_profile_metric(work_total, work_m, rank);
	reduce_profile_metric(sendrecv_total, sendrecv_m, rank);
	reduce_profile_metric(phase_total, phase_total_m, rank);
	reduce_profile_metric(verify_s, verify_m, rank);

	reduce_profile_array(phase_zero_s, zero_min, zero_sum, zero_max, phases);
	reduce_profile_array(phase_work_s, work_min, work_sum, work_max, phases);
	reduce_profile_array(phase_sendrecv_s, comm_min, comm_sum, comm_max, phases);
	reduce_profile_array(phase_total_s, phase_min, phase_sum, phase_max, phases);

	if (rank != 0)
		return;

	fprintf(stderr,
		"\n[MPI PROFILE] Rank aggregate rep=%d  ranks=%d  n=%d  phases=%d\n",
		rep, num_ranks, n, phases);
	fprintf(stderr, "  all times in ms; min/avg/max are across ranks\n");
	print_metric("pre-start barrier", barrier_m, num_ranks);
	print_metric("total wall", total_m, num_ranks);
	print_metric("zero state", zero_m, num_ranks);
	print_metric("user compute", work_m, num_ranks);
	print_metric("MPI_Sendrecv", sendrecv_m, num_ranks);
	print_metric("phase total", phase_total_m, num_ranks);
	print_metric("verify", verify_m, num_ranks);
	fprintf(stderr,
		"  sendrecv payload  : row_count=%d MPI_UNSIGNED  steps/phase=%d  bytes/phase/rank=%zu send + %zu recv  bytes/rep/rank=%zu send + %zu recv\n",
		n, num_ranks - 1, bytes_per_phase, bytes_per_phase,
		bytes_per_rank_total, bytes_per_rank_total);
	fprintf(stderr, "  per-phase breakdown:\n");
	for (p = 0; p < phases; p++) {
		fprintf(stderr,
			"    phase %2d  zero=%8.3f/%8.3f/%8.3f  "
			"compute=%8.3f/%8.3f/%8.3f  "
			"sendrecv=%8.3f/%8.3f/%8.3f  "
			"total=%8.3f/%8.3f/%8.3f\n",
			p,
			zero_min[p] * 1000.0,
			(zero_sum[p] / (double)num_ranks) * 1000.0,
			zero_max[p] * 1000.0,
			work_min[p] * 1000.0,
			(work_sum[p] / (double)num_ranks) * 1000.0,
			work_max[p] * 1000.0,
			comm_min[p] * 1000.0,
			(comm_sum[p] / (double)num_ranks) * 1000.0,
			comm_max[p] * 1000.0,
			phase_min[p] * 1000.0,
			(phase_sum[p] / (double)num_ranks) * 1000.0,
			phase_max[p] * 1000.0);
	}
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int phases = DEFAULT_PHASES;
	int reps = 1;
	int rep, p;
	int rank, num_ranks;
	unsigned int *rows;
	size_t total_cells;
	int do_profile;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
	do_profile = profile_enabled();

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
		double phase_zero_s[MAX_PHASES] = {0.0};
		double phase_work_s[MAX_PHASES] = {0.0};
		double phase_sendrecv_s[MAX_PHASES] = {0.0};
		double phase_total_s[MAX_PHASES] = {0.0};
		double barrier_start_s, barrier_s;
		double total_start_s, total_s;
		double verify_start_s, verify_s;
		unsigned long result_ms;
		int verify_errors;

		barrier_start_s = MPI_Wtime();
		MPI_Barrier(MPI_COMM_WORLD);
		barrier_s = MPI_Wtime() - barrier_start_s;
		total_start_s = MPI_Wtime();

		for (p = 0; p < phases; p++) {
			double phase_start_s = MPI_Wtime();
			double t_start_s;

			t_start_s = MPI_Wtime();
			memset(rows, 0, total_cells * sizeof(*rows));
			phase_zero_s[p] = MPI_Wtime() - t_start_s;

			t_start_s = MPI_Wtime();
			run_phase(my_row, n,
				  (unsigned int)rank, rep, p, 1);
			phase_work_s[p] = MPI_Wtime() - t_start_s;

			t_start_s = MPI_Wtime();
			exchange_rows_sendrecv(rows, n, rank, num_ranks, p);
			phase_sendrecv_s[p] = MPI_Wtime() - t_start_s;
			phase_total_s[p] = MPI_Wtime() - phase_start_s;
		}

		total_s = MPI_Wtime() - total_start_s;
		result_ms = (unsigned long)(total_s * 1000.0 + 0.5);

		verify_start_s = MPI_Wtime();
		verify_errors = verify(rows, n, num_ranks, rank, rep, phases - 1);
		verify_s = MPI_Wtime() - verify_start_s;
		if (verify_errors != 0) {
			free(rows);
			MPI_Abort(MPI_COMM_WORLD, 1);
			return 1;
		}
		if (rank == 0) {
			printf("RESULT n=%d phases=%d rep=%d ms=%lu\n",
			       n, phases, rep, result_ms);
			fflush(stdout);
		}
		if (do_profile)
			profile_report(rank, num_ranks, n, phases, rep,
				       barrier_s, total_s, verify_s,
				       phase_zero_s, phase_work_s,
				       phase_sendrecv_s, phase_total_s);
	}

	free(rows);
	MPI_Finalize();
	return 0;
}
