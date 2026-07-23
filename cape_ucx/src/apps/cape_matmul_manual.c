/*
 * Distributed dense matrix multiplication C = A x B — DICKPT version.
 *
 * The ultimate compute-bound benchmark: an N^3 inner loop with no
 * communication until the final exchange. Rows of C are block-distributed
 * across ranks; A and B are replicated read-only globals (same seed on every
 * rank), so they are NOT in the tracked region. Each rank computes its own
 * stripe of rows and writes them into the shared C; one allreduce unions the
 * disjoint row stripes (block pattern), after which every rank holds the full
 * product. Uses double precision so the FLOP-heavy core dominates the runtime.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sched.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 1024
#define MAX_N 2048

/* Read-only operands, replicated on every rank — not tracked. */
static double a[MAX_N][MAX_N];
static double b[MAX_N][MAX_N];

/* Result matrix: each rank writes only its row stripe (disjoint union). */
struct ckpt_state {
	double c[MAX_N][MAX_N];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

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

/* ── clone(2)-based compute threads ──────────────────────────────────────
 * Row-disjoint split of this rank's stripe [row_start,row_end): each worker
 * multiplies its own rows into state->c, so no synchronization is needed.
 * All VM-sharing flags ON (threads, not processes): one C matrix, one uffd.
 * Main thread runs slab 0 itself, so nt==1 is the original serial loop. */
#define MM_STACK_BYTES (1u << 20)

struct mm_task {
	int n;
	int row_lo, row_hi;          /* this worker's row range */
	const double (*a)[MAX_N];
	const double (*b)[MAX_N];
	double (*c)[MAX_N];
	int32_t join_futex;          /* nonzero while thread alive */
	char *stack;                 /* private mmap, reused every rep */
};

static void mm_kernel(struct mm_task *t)
{
	int n = t->n, i, j, k;
	if (getenv("CAPE_AFFINITY_DEBUG")) {
		fprintf(stderr, "matmul: worker rows %d..%d running on cpu %d\n",
			t->row_lo, t->row_hi, sched_getcpu());
		fflush(stderr);
	}
	for (i = t->row_lo; i < t->row_hi; i++) {
		double *ci = t->c[i];
		for (k = 0; k < n; k++) {
			double aik = t->a[i][k];
			const double *bk = t->b[k];
			for (j = 0; j < n; j++)
				ci[j] += aik * bk[j];
		}
	}
}

static int mm_worker(void *arg)
{
	mm_kernel((struct mm_task *)arg);
	return 0;
}

static int mm_thread_count(void)
{
	const char *e = getenv("CAPE_COMPUTE_THREADS");
	cpu_set_t set;
	int nt = 0;

	if (e != NULL && e[0] != '\0')
		nt = atoi(e);
	if (nt <= 0 && sched_getaffinity(0, sizeof(set), &set) == 0)
		nt = CPU_COUNT(&set);
	if (nt <= 0)
		nt = 1;
	return nt;
}

/* Run the multiply over [row_lo,row_hi) with nt workers. */
static void mm_run(struct mm_task *tasks, int nt, int row_lo, int row_hi)
{
	int rows = row_hi - row_lo;
	int t;

	if (rows <= 0)
		return;
	if (nt > rows)
		nt = rows;

	for (t = 0; t < nt; t++) {
		tasks[t].row_lo = row_lo + (int)(((long)rows * t) / nt);
		tasks[t].row_hi = row_lo + (int)(((long)rows * (t + 1)) / nt);
	}

	for (t = 1; t < nt; t++) {
		struct mm_task *w = &tasks[t];
		int flags = CLONE_VM | CLONE_FS | CLONE_FILES |
			    CLONE_SIGHAND | CLONE_THREAD |
			    CLONE_CHILD_CLEARTID;
		w->join_futex = 1;
		if (clone(mm_worker, w->stack + MM_STACK_BYTES, flags, w,
			  NULL, NULL, &w->join_futex) == -1) {
			w->join_futex = 0;
			fprintf(stderr, "matmul: clone failed (%s); "
				"running rows %d..%d inline\n",
				strerror(errno), w->row_lo, w->row_hi);
			mm_kernel(w);
		}
	}

	mm_kernel(&tasks[0]);

	for (t = 1; t < nt; t++) {
		struct mm_task *w = &tasks[t];
		int32_t v;
		while ((v = __atomic_load_n(&w->join_futex,
					    __ATOMIC_ACQUIRE)) != 0)
			syscall(SYS_futex, &w->join_futex, FUTEX_WAIT,
				v, NULL, NULL, 0);
	}
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

/* Spot-check a few cells of the local stripe against a recomputed reference. */
static int verify_sample(const struct ckpt_state *s, int n,
			 int row_start, int row_end, unsigned long node, int rep)
{
	int checks[4][2];
	int span = (row_end > row_start) ? (row_end - row_start) : 1;
	int t;

	checks[0][0] = row_start;        checks[0][1] = 0;
	checks[1][0] = row_start;        checks[1][1] = n - 1;
	checks[2][0] = row_end - 1;      checks[2][1] = n / 2;
	checks[3][0] = row_start + span / 2; checks[3][1] = n / 3;

	for (t = 0; t < 4; t++) {
		int i = checks[t][0], j = checks[t][1], k;
		double expected = 0.0;
		if (i < 0 || i >= n || j < 0 || j >= n)
			continue;
		for (k = 0; k < n; k++)
			expected += a[i][k] * b[k][j];
		if (fabs(s->c[i][j] - expected) > 1e-6 * (fabs(expected) + 1.0)) {
			fprintf(stderr,
				"VERIFY FAIL node=%lu rep=%d c[%d][%d] got=%.6f expected=%.6f\n",
				node, rep, i, j, s->c[i][j], expected);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int reps = 1;
	int rep;
	unsigned long node, num_nodes;
	int row_start, row_end;
	struct ckpt_state *state = &g_state;
	unsigned long long rng = 0xdeadbeefcafebabeULL;
	int i, j, k;

	if (argc > 1)
		n = atoi(argv[1]);
	if (argc > 2)
		reps = atoi(argv[2]);
	if (n <= 0 || n > MAX_N) {
		fprintf(stderr, "ERROR: n must be in [1, %d], got %d\n", MAX_N, n);
		return 1;
	}
	if (reps <= 0)
		reps = 1;

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();

	row_start = (int)(((long)n * node) / num_nodes);
	row_end = (int)(((long)n * (node + 1)) / num_nodes);

	/* Same seed on every rank => identical replicated A and B. */
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++) {
			a[i][j] = to_unit(next_rand64(&rng));
			b[i][j] = to_unit(next_rand64(&rng));
		}

	memset(state, 0, sizeof(*state));
	dickpt_register_region(state, sizeof(*state));

	/* Compute-thread pool: shared task descriptors + private stacks,
	 * allocated before tracking starts so thread-private state never lands
	 * in a checkpoint. Cap threads to the rows this rank owns. */
	int nthreads = mm_thread_count();
	int my_rows = row_end - row_start;
	if (my_rows < 1)
		my_rows = 1;
	if (nthreads > my_rows)
		nthreads = my_rows;
	struct mm_task *tasks = calloc(nthreads, sizeof(*tasks));
	if (tasks == NULL) {
		fprintf(stderr, "node %lu: task alloc failed\n", node);
		return 1;
	}
	for (i = 0; i < nthreads; i++) {
		tasks[i].n = n;
		tasks[i].a = a;
		tasks[i].b = b;
		tasks[i].c = state->c;
		if (i == 0)
			continue;   /* main thread needs no stack */
		tasks[i].stack = mmap(NULL, MM_STACK_BYTES,
				      PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
				      -1, 0);
		if (tasks[i].stack == MAP_FAILED) {
			fprintf(stderr, "node %lu: stack mmap failed (%s); "
				"using %d threads\n",
				node, strerror(errno), i);
			nthreads = i;
			break;
		}
	}
	if (node == 0) {
		cpu_set_t aff;
		int ncpu = -1;
		if (sched_getaffinity(0, sizeof(aff), &aff) == 0)
			ncpu = CPU_COUNT(&aff);
		printf("matmul: %d compute thread(s) per node, "
		       "affinity=%d cpus\n", nthreads, ncpu);
	}

	dickpt_send_num_jobs((unsigned long)n);

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1, ts;
		unsigned long compute_us = 0, ckpt_us = 0;

		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				state->c[i][j] = 0.0;

		t0 = get_ms_of_day();

		ts = get_us_of_day();
		dickpt_start_ckpt();
		ckpt_us += get_us_of_day() - ts;

		ts = get_us_of_day();
		mm_run(tasks, nthreads, row_start, row_end);
		compute_us += get_us_of_day() - ts;

		ts = get_us_of_day();
		dickpt_generate_ckpt();
		dickpt_allreduce_ckpt();
		dickpt_stop_ckpt();
		ckpt_us += get_us_of_day() - ts;

		t1 = get_ms_of_day();

		if (verify_sample(state, n, row_start, row_end, node, rep) != 0)
			return 1;

		if (node == 0)
			printf("RESULT n=%d rep=%d ms=%lu "
			       "compute_ms=%lu ckpt_ms=%lu\n",
			       n, rep, t1 - t0,
			       compute_us / 1000UL, ckpt_us / 1000UL);
	}

	return 0;
}
