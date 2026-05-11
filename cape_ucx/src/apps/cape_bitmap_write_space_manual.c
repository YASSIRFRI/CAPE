/*
 * Sparse random SUM reductions over a large global grid, DICKPT bitmap version.
 *
 * The grid is declared as one SUM reduction range. Each rank writes a tiny
 * random subset of cells; the bitmap monitor captures only dirty words/pages
 * and merges matching cells with SUM.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "../../include/cape_dickpt.h"

#define DEFAULT_GRID_BYTES (2ull * 1024ull * 1024ull * 1024ull)
#define DEFAULT_UPDATES 5000ul
#define DEFAULT_PHASES 1

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

static uint64_t mix64(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

static uint64_t seed_for(unsigned int rank, int rep, int phase)
{
	return 0x6a09e667f3bcc909ULL ^
	       ((uint64_t)(rank + 1U) * 0xbf58476d1ce4e5b9ULL) ^
	       ((uint64_t)rep * 0x94d049bb133111ebULL) ^
	       ((uint64_t)(phase + 1) * 0xd1b54a32d192ed03ULL);
}

static size_t parse_size_arg(const char *text, size_t fallback)
{
	char *end = NULL;
	unsigned long long value;
	unsigned long long scale = 1;

	if (text == NULL || text[0] == '\0')
		return fallback;
	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno != 0 || end == text)
		return fallback;
	if (*end == 'k' || *end == 'K') {
		scale = 1024ull;
		end++;
	} else if (*end == 'm' || *end == 'M') {
		scale = 1024ull * 1024ull;
		end++;
	} else if (*end == 'g' || *end == 'G') {
		scale = 1024ull * 1024ull * 1024ull;
		end++;
	}
	if (*end != '\0' || value > (unsigned long long)SIZE_MAX / scale)
		return fallback;
	return (size_t)(value * scale);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t av = *(const uint64_t *)a;
	uint64_t bv = *(const uint64_t *)b;
	return (av > bv) - (av < bv);
}

static unsigned long update_value(uint64_t *rng)
{
	return (unsigned long)(0x100000000ULL |
			       ((next_rand64(rng) % 97u) + 1u));
}

static void replay_updates(uint64_t *indices, size_t *n_indices,
			   unsigned long num_nodes, unsigned long updates,
			   size_t grid_cells, int rep, int phase,
			   uint64_t *sum_out, uint64_t *weighted_out)
{
	uint64_t sum = 0;
	uint64_t weighted = 0;
	size_t pos = 0;
	unsigned long r;

	for (r = 0; r < num_nodes; r++) {
		uint64_t rng = seed_for((unsigned int)r, rep, phase);
		unsigned long u;

		for (u = 0; u < updates; u++) {
			uint64_t idx = next_rand64(&rng) % (uint64_t)grid_cells;
			unsigned long value = update_value(&rng);

			indices[pos++] = idx;
			sum += (uint64_t)value;
			weighted += (uint64_t)value * mix64(idx);
		}
	}

	*n_indices = pos;
	if (sum_out)
		*sum_out = sum;
	if (weighted_out)
		*weighted_out = weighted;
}

static void zero_union(unsigned long *grid, uint64_t *indices, size_t n_indices)
{
	size_t i;

	qsort(indices, n_indices, sizeof(indices[0]), cmp_u64);
	for (i = 0; i < n_indices; i++) {
		if (i != 0 && indices[i] == indices[i - 1])
			continue;
		grid[indices[i]] = 0;
	}
}

static void clear_phase(unsigned long *grid, uint64_t *indices,
			unsigned long num_nodes, unsigned long updates,
			size_t grid_cells, int rep, int phase)
{
	size_t n_indices = 0;

	replay_updates(indices, &n_indices, num_nodes, updates, grid_cells,
		       rep, phase, NULL, NULL);
	zero_union(grid, indices, n_indices);
}

static void apply_local_updates(unsigned long *grid, size_t grid_cells,
				unsigned int rank, unsigned long updates,
				int rep, int phase)
{
	uint64_t rng = seed_for(rank, rep, phase);
	unsigned long u;

	for (u = 0; u < updates; u++) {
		uint64_t idx = next_rand64(&rng) % (uint64_t)grid_cells;
		unsigned long value = update_value(&rng);

		grid[idx] += value;
	}
}

static int verify_phase(const unsigned long *grid, uint64_t *indices,
			unsigned long num_nodes, unsigned long node,
			unsigned long updates, size_t grid_cells,
			int rep, int phase)
{
	uint64_t expected_sum = 0, expected_weighted = 0;
	uint64_t actual_sum = 0, actual_weighted = 0;
	size_t n_indices = 0;
	size_t i;

	replay_updates(indices, &n_indices, num_nodes, updates, grid_cells,
		       rep, phase, &expected_sum, &expected_weighted);
	qsort(indices, n_indices, sizeof(indices[0]), cmp_u64);

	for (i = 0; i < n_indices; i++) {
		uint64_t idx = indices[i];
		uint64_t value;

		if (i != 0 && idx == indices[i - 1])
			continue;
		value = (uint64_t)grid[idx];
		actual_sum += value;
		actual_weighted += value * mix64(idx);
	}

	if (actual_sum != expected_sum || actual_weighted != expected_weighted) {
		fprintf(stderr,
			"VERIFY FAIL node=%lu rep=%d phase=%d sum=0x%016llx expected=0x%016llx weighted=0x%016llx expected_weighted=0x%016llx\n",
			node, rep, phase,
			(unsigned long long)actual_sum,
			(unsigned long long)expected_sum,
			(unsigned long long)actual_weighted,
			(unsigned long long)expected_weighted);
		return 1;
	}

	if (node == 0)
		printf("VERIFY OK  rep=%d phase=%d (%lu ranks x %lu sparse updates, cells=%zu)\n",
		       rep, phase, num_nodes, updates, grid_cells);
	return 0;
}

int main(int argc, char *argv[])
{
	size_t grid_bytes = DEFAULT_GRID_BYTES;
	unsigned long updates = DEFAULT_UPDATES;
	int phases = DEFAULT_PHASES;
	int reps = 1;
	unsigned long node, num_nodes;
	unsigned long *grid;
	uint64_t *indices;
	size_t grid_cells;
	size_t max_indices;
	int rep, p;
	int have_dirty = 0, dirty_rep = 0, dirty_phase = 0;

	if (argc > 1)
		grid_bytes = parse_size_arg(argv[1], DEFAULT_GRID_BYTES);
	if (argc > 2)
		updates = strtoul(argv[2], NULL, 0);
	if (argc > 3)
		phases = atoi(argv[3]);
	if (argc > 4)
		reps = atoi(argv[4]);
	if (updates == 0)
		updates = DEFAULT_UPDATES;
	if (phases <= 0)
		phases = 1;
	if (reps <= 0)
		reps = 1;

	grid_bytes -= grid_bytes % sizeof(*grid);
	grid_cells = grid_bytes / sizeof(*grid);
	if (grid_cells == 0) {
		fprintf(stderr, "ERROR: grid_bytes is too small\n");
		return 1;
	}
	if (grid_bytes > UINT_MAX) {
		fprintf(stderr,
			"ERROR: grid_bytes must be <= %u for this bitmap monitor, got %zu\n",
			UINT_MAX, grid_bytes);
		return 1;
	}

	grid = dickpt_map_region(grid_bytes);
	if (grid == NULL) {
		fprintf(stderr, "ERROR: failed to map grid_bytes=%zu\n", grid_bytes);
		return 1;
	}
	dickpt_declare_reduction_region(grid, grid_bytes,
					CAPE_UNSIGNED_LONG, D_REDUCTION_SUM);

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();
	if (num_nodes == 0) {
		fprintf(stderr, "ERROR: num_nodes is zero\n");
		return 1;
	}
	if (num_nodes > SIZE_MAX / updates) {
		fprintf(stderr, "ERROR: too many sparse updates\n");
		return 1;
	}
	max_indices = (size_t)num_nodes * (size_t)updates;
	indices = malloc(max_indices * sizeof(*indices));
	if (indices == NULL) {
		fprintf(stderr, "ERROR: failed to allocate %zu indices\n", max_indices);
		return 1;
	}

	dickpt_send_num_jobs(updates);

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;

		if (have_dirty) {
			clear_phase(grid, indices, num_nodes, updates, grid_cells,
				    dirty_rep, dirty_phase);
			have_dirty = 0;
		}

		t0 = get_ms_of_day();

		for (p = 0; p < phases; p++) {
			if (have_dirty) {
				clear_phase(grid, indices, num_nodes, updates,
					    grid_cells, dirty_rep, dirty_phase);
				have_dirty = 0;
			}

			dickpt_start_ckpt();
			apply_local_updates(grid, grid_cells, (unsigned int)node,
					    updates, rep, p);
			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();

			have_dirty = 1;
			dirty_rep = rep;
			dirty_phase = p;
		}

		t1 = get_ms_of_day();

		if (verify_phase(grid, indices, num_nodes, node, updates,
				 grid_cells, rep, phases - 1) != 0) {
			free(indices);
			return 1;
		}

		if (node == 0) {
			printf("RESULT grid_bytes=%zu updates=%lu phases=%d rep=%d ms=%lu\n",
			       grid_bytes, updates, phases, rep, t1 - t0);
			printf("PAYLOAD sparse_updates_per_rank=%lu reduction_range_bytes=%zu\n",
			       updates, grid_bytes);
		}
	}

	free(indices);
	return 0;
}
