/*
 * Sparse Game of Life over a huge global board, DICKPT bitmap version.
 *
 * Each node owns a horizontal stripe of live cells. For each generation it
 * contributes Moore-neighborhood counts into one replicated global count grid.
 * The count grid is declared as an unsigned-int SUM reduction range, letting
 * cape_incr_bitmap merge only dirty count words.
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
#define DEFAULT_LIVE_PER_RANK 5000ul
#define DEFAULT_PHASES 1
#define DEFAULT_WIDTH 16384ul

struct live_set {
	uint64_t *idx;
	size_t count;
	size_t cap;
};

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

static uint64_t seed_for(unsigned int rank, int rep)
{
	return 0x243f6a8885a308d3ULL ^
	       ((uint64_t)(rank + 1U) * 0x9e3779b97f4a7c15ULL) ^
	       ((uint64_t)rep * 0xbf58476d1ce4e5b9ULL);
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

static int live_set_reserve(struct live_set *set, size_t want)
{
	uint64_t *p;
	size_t cap = set->cap ? set->cap : 1024;

	if (want <= set->cap)
		return 0;
	while (cap < want) {
		if (cap > SIZE_MAX / 2)
			return 1;
		cap *= 2;
	}
	p = realloc(set->idx, cap * sizeof(*set->idx));
	if (p == NULL)
		return 1;
	set->idx = p;
	set->cap = cap;
	return 0;
}

static int live_set_append(struct live_set *set, uint64_t idx)
{
	if (set->count == set->cap &&
	    live_set_reserve(set, set->count + 1) != 0)
		return 1;
	set->idx[set->count++] = idx;
	return 0;
}

static void choose_shape(size_t *grid_bytes, size_t *width_out,
			 size_t *height_out, size_t *cells_out)
{
	size_t cells = *grid_bytes / sizeof(unsigned int);
	size_t width = DEFAULT_WIDTH;
	size_t height;

	while (width > 1024 && cells / width < 128)
		width /= 2;
	height = cells / width;
	cells = height * width;
	*grid_bytes = cells * sizeof(unsigned int);
	*width_out = width;
	*height_out = height;
	*cells_out = cells;
}

static void stripe_for_node(unsigned long node, unsigned long nnodes,
			    size_t height, size_t *row_start, size_t *row_end)
{
	*row_start = ((size_t)node * height) / (size_t)nnodes;
	*row_end = ((size_t)(node + 1) * height) / (size_t)nnodes;
}

static int add_live_cell(unsigned char *live, size_t row_start, size_t width,
			 size_t row, size_t col, struct live_set *set)
{
	size_t local = (row - row_start) * width + col;
	uint64_t global = (uint64_t)row * (uint64_t)width + col;

	if (live[local])
		return 0;
	live[local] = 1;
	if (live_set_append(set, global) != 0)
		return -1;
	return 1;
}

static int init_live(unsigned char *live, size_t local_cells,
		     size_t row_start, size_t row_end, size_t width,
		     size_t height, unsigned long target,
		     unsigned int node, int rep, struct live_set *set)
{
	uint64_t rng = seed_for(node, rep);
	size_t row_min = row_start == 0 ? 1 : row_start;
	size_t row_max = row_end == height ? height - 1 : row_end;
	size_t row_span;
	unsigned long attempts = 0;
	unsigned long max_attempts = target * 20ul + 1000ul;

	memset(live, 0, local_cells);
	set->count = 0;
	if (height < 3 || width < 3 || row_min >= row_max)
		return 0;

	row_span = row_max - row_min;
	while (set->count < (size_t)target && attempts++ < max_attempts) {
		size_t row = row_min + (size_t)(next_rand64(&rng) % row_span);
		size_t col = 2 + (size_t)(next_rand64(&rng) % (width - 4));
		int off;

		for (off = -1; off <= 1 && set->count < (size_t)target; off++) {
			if (add_live_cell(live, row_start, width, row,
					  (size_t)((long)col + off), set) < 0)
				return 1;
		}
	}
	return 0;
}

static void contribute_counts(unsigned int *counts, size_t width, size_t height,
			      const struct live_set *live_cells)
{
	size_t i;

	for (i = 0; i < live_cells->count; i++) {
		uint64_t idx = live_cells->idx[i];
		size_t row = (size_t)(idx / width);
		size_t col = (size_t)(idx - (uint64_t)row * width);
		int dr, dc;

		for (dr = -1; dr <= 1; dr++) {
			long nr = (long)row + dr;
			if (nr < 0 || (size_t)nr >= height)
				continue;
			for (dc = -1; dc <= 1; dc++) {
				long nc = (long)col + dc;
				size_t nidx;
				if (dr == 0 && dc == 0)
					continue;
				if (nc < 0 || (size_t)nc >= width)
					continue;
				nidx = (size_t)nr * width + (size_t)nc;
				counts[nidx]++;
			}
		}
	}
}

static void touch_count_pages(unsigned int *counts, size_t width, size_t height,
			      const struct live_set *live_cells)
{
	size_t i;

	for (i = 0; i < live_cells->count; i++) {
		uint64_t idx = live_cells->idx[i];
		size_t row = (size_t)(idx / width);
		size_t col = (size_t)(idx - (uint64_t)row * width);
		int dr, dc;

		for (dr = -1; dr <= 1; dr++) {
			long nr = (long)row + dr;
			if (nr < 0 || (size_t)nr >= height)
				continue;
			for (dc = -1; dc <= 1; dc++) {
				long nc = (long)col + dc;
				size_t nidx;
				volatile unsigned int *cell;
				if (dr == 0 && dc == 0)
					continue;
				if (nc < 0 || (size_t)nc >= width)
					continue;
				nidx = (size_t)nr * width + (size_t)nc;
				cell = &counts[nidx];
				*cell = *cell;
			}
		}
	}
}

static int advance_generation(const unsigned int *counts,
			      const unsigned char *live,
			      unsigned char *next_live,
			      size_t row_start, size_t local_cells,
			      size_t width, struct live_set *next_cells)
{
	size_t i;

	memset(next_live, 0, local_cells);
	next_cells->count = 0;
	for (i = 0; i < local_cells; i++) {
		size_t global = row_start * width + i;
		unsigned int n = counts[global];
		int alive = live[i] != 0;

		if (n == 3 || (alive && n == 2)) {
			next_live[i] = 1;
			if (live_set_append(next_cells, (uint64_t)global) != 0)
				return 1;
		}
	}
	return 0;
}

static uint64_t checksum_live(const struct live_set *set)
{
	uint64_t h = 0;
	size_t i;

	for (i = 0; i < set->count; i++)
		h ^= mix64(set->idx[i]);
	return h;
}

int main(int argc, char **argv)
{
	size_t grid_bytes = DEFAULT_GRID_BYTES;
	unsigned long live_per_rank = DEFAULT_LIVE_PER_RANK;
	int phases = DEFAULT_PHASES;
	int reps = 1;
	unsigned long node, nnodes;
	size_t width, height, total_cells;
	size_t row_start, row_end, local_rows, local_cells;
	unsigned int *counts = NULL;
	unsigned char *live = NULL, *next_live = NULL;
	struct live_set live_cells = {0}, next_cells = {0};
	int rep, p;

	if (argc > 1)
		grid_bytes = parse_size_arg(argv[1], DEFAULT_GRID_BYTES);
	if (argc > 2)
		live_per_rank = strtoul(argv[2], NULL, 0);
	if (argc > 3)
		phases = atoi(argv[3]);
	if (argc > 4)
		reps = atoi(argv[4]);
	if (live_per_rank == 0)
		live_per_rank = DEFAULT_LIVE_PER_RANK;
	if (phases <= 0)
		phases = 1;
	if (reps <= 0)
		reps = 1;

	node = dickpt_read_node();
	nnodes = dickpt_read_num_nodes();
	if (nnodes == 0) {
		fprintf(stderr, "ERROR: num_nodes is zero\n");
		return 1;
	}

	choose_shape(&grid_bytes, &width, &height, &total_cells);
	if (height < (size_t)nnodes || total_cells == 0) {
		fprintf(stderr,
			"ERROR: grid too small for %lu nodes (cells=%zu height=%zu)\n",
			nnodes, total_cells, height);
		return 1;
	}
	if (grid_bytes > UINT_MAX) {
		fprintf(stderr,
			"ERROR: grid_bytes must be <= %u for this bitmap monitor, got %zu\n",
			UINT_MAX, grid_bytes);
		return 1;
	}

	stripe_for_node(node, nnodes, height, &row_start, &row_end);
	local_rows = row_end - row_start;
	local_cells = local_rows * width;

	counts = dickpt_map_region(grid_bytes);
	if (counts == NULL) {
		fprintf(stderr, "ERROR: failed to map grid_bytes=%zu\n", grid_bytes);
		return 1;
	}
	dickpt_declare_reduction_region(counts, grid_bytes,
					CAPE_UNSIGNED_INT, D_REDUCTION_SUM);

	live = calloc(local_cells, sizeof(*live));
	next_live = calloc(local_cells, sizeof(*next_live));
	if (live == NULL || next_live == NULL ||
	    live_set_reserve(&live_cells, (size_t)live_per_rank + 1024) != 0 ||
	    live_set_reserve(&next_cells, (size_t)live_per_rank * 16 + 1024) != 0) {
		fprintf(stderr, "node %lu: allocation failed\n", node);
		free(live);
		free(next_live);
		free(live_cells.idx);
		free(next_cells.idx);
		return 1;
	}

	dickpt_send_num_jobs(live_per_rank);

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;

		if (init_live(live, local_cells, row_start, row_end, width, height,
			      live_per_rank, (unsigned int)node, rep,
			      &live_cells) != 0) {
			fprintf(stderr, "node %lu: failed to initialize live set\n", node);
			return 1;
		}

		t0 = get_ms_of_day();

		for (p = 0; p < phases; p++) {
			unsigned char *tmp_live;
			struct live_set tmp_set;

			if (p != 0 || rep != 1)
				memset(counts, 0, grid_bytes);

			touch_count_pages(counts, width, height, &live_cells);
			dickpt_start_ckpt();
			contribute_counts(counts, width, height, &live_cells);
			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();

			if (advance_generation(counts, live, next_live, row_start,
					       local_cells, width, &next_cells) != 0) {
				fprintf(stderr, "node %lu: failed to advance generation\n", node);
				return 1;
			}

			tmp_live = live;
			live = next_live;
			next_live = tmp_live;
			tmp_set = live_cells;
			live_cells = next_cells;
			next_cells = tmp_set;
		}

		t1 = get_ms_of_day();

		if (node == 0) {
			printf("VERIFY LOCAL OK  node=%lu rep=%d phases=%d live=%zu checksum=0x%016llx\n",
			       node, rep, phases, live_cells.count,
			       (unsigned long long)checksum_live(&live_cells));
			printf("RESULT grid_bytes=%zu cells=%zu width=%zu height=%zu live_per_rank=%lu phases=%d rep=%d ms=%lu\n",
			       grid_bytes, total_cells, width, height,
			       live_per_rank, phases, rep, t1 - t0);
			printf("PAYLOAD sparse_count_updates_per_rank=%lu reduction_range_bytes=%zu stencil=moore8\n",
			       live_per_rank * 8ul, grid_bytes);
			fflush(stdout);
		}
	}

	free(live);
	free(next_live);
	free(live_cells.idx);
	free(next_cells.idx);
	return 0;
}
