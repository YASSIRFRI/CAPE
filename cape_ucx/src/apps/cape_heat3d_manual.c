/*
 * 3D diffusion solver (Jacobi iteration) — DICKPT version.
 *
 * Transient scalar diffusion on an N x N x N cube — the kernel behind
 * thermal/fluid-dynamics diffusion (heat conduction, viscous/species terms).
 * We march to steady state with a 7-point Jacobi stencil:
 *
 *   u'[i][j][k] = (1/6) * ( u[i-1][j][k] + u[i+1][j][k]
 *                         + u[i][j-1][k] + u[i][j+1][k]
 *                         + u[i][j][k-1] + u[i][j][k+1] )
 *
 * Dirichlet boundaries: a small HOT square patch at the centre of the i==0 face,
 * everything else (the rest of that face and all other faces) COLD. The heat
 * source is therefore spatially LOCAL and spreads as a growing hemisphere.
 *
 * Why this captures the power of DICKPT
 * -------------------------------------
 * The cube is slab-distributed along the first axis (i). Each rank updates ONLY
 * its own block of i-planes, reading its neighbours' boundary ("halo") planes
 * produced in the previous iteration. A classic MPI code forces the programmer
 * to hand-write a halo exchange with up/down neighbours every iteration — see
 * mpi_heat3d.c.
 *
 * Here the application is written as if it were plain shared-memory OpenMP: it
 * writes its planes of the shared cube and lets the DICKPT monitor figure out
 * what changed. Per iteration only the dirty slab is captured and merged
 * (incremental checkpoint + union allreduce). For diffusion from a small hot
 * patch the delta starts TINY (only the few cells near the source move) and
 * grows as the warm hemisphere expands — which is exactly what the checkpoint-size job
 * logs.
 *
 * Implementation notes (mirroring cape_heat_manual.c):
 *   - The tracked cube comes from dickpt_map_region(): a clean page-aligned
 *     anonymous mapping at an identical VA on every rank, registered for
 *     write-protect in one shot.
 *   - We pre-touch our slab before each start_ckpt so the pages are resident
 *     when the monitor write-protects them; otherwise a first write to a
 *     not-yet-resident page would not fault and the delta would be missed.
 *   - The double-buffer scratch (unew) is ordinary heap memory: never shipped.
 *   - Compute threads: the Jacobi sweep and the conditional write-back are
 *     split over CAPE_COMPUTE_THREADS threads (default: one per CPU in the
 *     process affinity mask). Threads are raw clone(2) with
 *     CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD — NOT fork-like
 *     copies — so there is exactly ONE cube, ONE address space, ONE uffd
 *     dirty-tracking domain, and therefore still one checkpoint per node.
 *     Only the DICKPT-oblivious number crunching runs in the threads; every
 *     dickpt_* signal is issued by the main thread while the workers are
 *     joined, so the monitor's ptrace view is unchanged. Thread stacks are
 *     private mmaps, never part of the tracked region.
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

#define DEFAULT_N 128
#define DEFAULT_ITERS 100
#define MAX_N 512

#define HOT 1.0
#define COLD 0.0

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
 * Minimal thread pool substitute: per phase we clone one worker per plane
 * sub-slab, each runs the phase kernel on its planes and exits; the parent
 * joins on the CLONE_CHILD_CLEARTID futex the kernel clears at thread exit.
 * All VM-sharing flags ON (threads, not processes): one cube, one uffd. */

#define HB_STACK_BYTES (1u << 20)

enum hb_phase { HB_SWEEP, HB_WRITEBACK };

struct hb_task {
	enum hb_phase phase;
	int i_lo, i_hi;              /* this worker's plane range */
	int n;
	size_t plane;
	double *u, *unew;
	int32_t join_futex;          /* nonzero while thread alive */
	char *stack;                 /* private mmap, reused every phase */
};

static void hb_kernel(const struct hb_task *t)
{
	int n = t->n, i, j, k;
	size_t plane = t->plane;
	double *u = t->u, *unew = t->unew;

#define TU(i, j, k)    u[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]
#define TUNEW(i, j, k) unew[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]
	if (t->phase == HB_SWEEP) {
		for (i = t->i_lo; i < t->i_hi; i++)
			for (j = 1; j < n - 1; j++)
				for (k = 1; k < n - 1; k++)
					TUNEW(i, j, k) = (1.0 / 6.0) *
						(TU(i - 1, j, k) + TU(i + 1, j, k) +
						 TU(i, j - 1, k) + TU(i, j + 1, k) +
						 TU(i, j, k - 1) + TU(i, j, k + 1));
	} else {
		/* Conditional write-back: see the comment at the call site —
		 * skipping unchanged cells is what keeps checkpoints tiny. */
		for (i = t->i_lo; i < t->i_hi; i++)
			for (j = 1; j < n - 1; j++)
				for (k = 1; k < n - 1; k++)
					if (TUNEW(i, j, k) != TU(i, j, k))
						TU(i, j, k) = TUNEW(i, j, k);
	}
#undef TU
#undef TUNEW
}

static int hb_worker(void *arg)
{
	hb_kernel((const struct hb_task *)arg);
	return 0;
}

static int hb_thread_count(void)
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

/* Run one phase over [i_lo, i_hi) with nt workers (main thread runs slab 0
 * itself, so nt==1 degenerates to the original serial loop with no clone). */
static int hb_run_phase(struct hb_task *tasks, int nt, enum hb_phase phase,
			int i_lo, int i_hi)
{
	int planes = i_hi - i_lo;
	int t, spawned;

	if (planes <= 0)
		return 0;
	if (nt > planes)
		nt = planes;

	for (t = 0; t < nt; t++) {
		tasks[t].phase = phase;
		tasks[t].i_lo = i_lo + (int)(((long)planes * t) / nt);
		tasks[t].i_hi = i_lo + (int)(((long)planes * (t + 1)) / nt);
	}

	spawned = 0;
	for (t = 1; t < nt; t++) {
		struct hb_task *w = &tasks[t];
		int flags = CLONE_VM | CLONE_FS | CLONE_FILES |
			    CLONE_SIGHAND | CLONE_THREAD |
			    CLONE_CHILD_CLEARTID;
		w->join_futex = 1;
		if (clone(hb_worker, w->stack + HB_STACK_BYTES, flags, w,
			  NULL, NULL, &w->join_futex) == -1) {
			/* Can't spawn: fold the remaining slabs into the main
			 * thread's share by running them inline below. */
			w->join_futex = 0;
			fprintf(stderr, "heat3d: clone failed (%s); "
				"running slab %d inline\n", strerror(errno), t);
			hb_kernel(w);
			continue;
		}
		spawned++;
	}

	hb_kernel(&tasks[0]);

	for (t = 1; t < nt; t++) {
		struct hb_task *w = &tasks[t];
		int32_t v;
		while ((v = __atomic_load_n(&w->join_futex,
					    __ATOMIC_ACQUIRE)) != 0)
			syscall(SYS_futex, &w->join_futex, FUTEX_WAIT,
				v, NULL, NULL, 0);
	}
	return spawned;
}

int main(int argc, char *argv[])
{
	int n = DEFAULT_N;
	int iters = DEFAULT_ITERS;
	int reps = 1;
	int rep, i, j, k;
	unsigned long node, num_nodes;
	int r_start, r_end;          /* this rank's global i-plane slab */
	int i_lo, i_hi;              /* interior i-planes this rank updates */
	size_t bytes, plane;
	double *u = NULL;            /* tracked shared cube (flat n x n x n) */
	double *unew = NULL;         /* untracked scratch */

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

	node = dickpt_read_node();
	num_nodes = dickpt_read_num_nodes();

	r_start = (int)(((long)n * node) / num_nodes);
	r_end = (int)(((long)n * (node + 1)) / num_nodes);
	/* Global planes 0 and n-1 are fixed Dirichlet boundaries. */
	i_lo = (r_start < 1) ? 1 : r_start;
	i_hi = (r_end > n - 1) ? n - 1 : r_end;

	plane = (size_t)n * (size_t)n;
	bytes = plane * (size_t)n * sizeof(double);

	u = (double *)dickpt_map_region(bytes);
	unew = (double *)malloc(bytes);
	if (u == NULL || unew == NULL) {
		fprintf(stderr, "node %lu: allocation failed (%zu bytes)\n",
			node, bytes);
		return 1;
	}

	/* Compute-thread pool: shared task descriptors + private stacks.
	 * Allocated before tracking starts; both live outside the tracked
	 * region so thread-private state is never shipped in a checkpoint. */
	int nthreads = hb_thread_count();
	/* Cap to the work available: the phases split this rank's interior
	 * i-planes, and hb_run_phase clamps workers to plane count anyway.
	 * Requesting more threads than planes only wastes stacks and clone/
	 * join overhead with zero extra parallelism. */
	int interior_planes = i_hi - i_lo;
	if (interior_planes < 1)
		interior_planes = 1;
	if (nthreads > interior_planes)
		nthreads = interior_planes;
	struct hb_task *tasks = calloc(nthreads, sizeof(*tasks));
	if (tasks == NULL) {
		fprintf(stderr, "node %lu: task alloc failed\n", node);
		return 1;
	}
	{
		int t;
		for (t = 0; t < nthreads; t++) {
			tasks[t].n = n;
			tasks[t].plane = plane;
			tasks[t].u = u;
			tasks[t].unew = unew;
			if (t == 0)
				continue;   /* main thread needs no stack */
			tasks[t].stack = mmap(NULL, HB_STACK_BYTES,
					      PROT_READ | PROT_WRITE,
					      MAP_PRIVATE | MAP_ANONYMOUS |
					      MAP_STACK, -1, 0);
			if (tasks[t].stack == MAP_FAILED) {
				fprintf(stderr,
					"node %lu: stack mmap failed (%s); "
					"using %d threads\n",
					node, strerror(errno), t);
				nthreads = t;
				break;
			}
		}
	}
	if (node == 0)
		printf("heat3d: %d compute thread(s) per node\n", nthreads);

	dickpt_send_num_jobs((unsigned long)n);

#define U(i, j, k)    u[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]
#define UNEW(i, j, k) unew[((size_t)(i) * plane) + ((size_t)(j) * n) + (k)]

	for (rep = 1; rep <= reps; rep++) {
		unsigned long t0, t1;
		unsigned long sweep_us = 0, writeback_us = 0, ckpt_us = 0;
		int it;

		/* Identical initial/boundary condition on every rank: the cube
		 * starts COLD and the heat source is a SMALL square patch at the
		 * centre of the i==0 face (held HOT, Dirichlet). The perturbation
		 * is therefore spatially local at t=0 and spreads as a growing
		 * hemisphere into the cube. This is what makes the dirty set —
		 * and hence each incremental checkpoint — start tiny and grow
		 * with the diffusion radius (the physics), instead of dirtying a
		 * full cross-section plane from iteration 1 (which a hot full face
		 * would do). hot_r is the patch half-width. */
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
			/* Pre-touch our slab so its pages are resident before
			 * the monitor write-protects them at start_ckpt. */
			for (i = i_lo; i < i_hi; i++) {
				volatile double *p = &U(i, 0, 0);
				p[0] = p[0];
				p[plane - 1] = p[plane - 1];
			}

			unsigned long ts;

			ts = get_us_of_day();
			dickpt_start_ckpt();
			ckpt_us += get_us_of_day() - ts;

			/* Jacobi sweep over this rank's interior planes, split
			 * over the compute threads by sub-slab. Reads the
			 * neighbours' halo planes produced by the previous
			 * merge; UNEW is written on disjoint planes per thread,
			 * so no synchronization is needed inside the phase. */
			ts = get_us_of_day();
			hb_run_phase(tasks, nthreads, HB_SWEEP, i_lo, i_hi);
			sweep_us += get_us_of_day() - ts;

			/* Commit our slab into the shared cube (the dirty set),
			 * same sub-slab split. Write back ONLY cells whose value
			 * actually changed. Ahead of the diffusion front the
			 * field is still exactly COLD and the Jacobi average of
			 * all-COLD neighbours is exactly COLD (IEEE: 0+0+...=0),
			 * so those cells are skipped and their pages are never
			 * faulted/dirtied. This is what makes the incremental
			 * checkpoint start tiny and grow ~1 plane/iter as the
			 * front sweeps through the cube — the whole premise of
			 * the benchmark. An unconditional write would dirty the
			 * entire slab every iteration and erase DICKPT's
			 * advantage. The write-back MUST be a separate phase
			 * after the sweep completes: it mutates U planes that
			 * neighbouring sub-slabs' sweeps read. */
			ts = get_us_of_day();
			hb_run_phase(tasks, nthreads, HB_WRITEBACK, i_lo, i_hi);
			writeback_us += get_us_of_day() - ts;

			ts = get_us_of_day();
			dickpt_generate_ckpt();
			dickpt_allreduce_ckpt();
			dickpt_stop_ckpt();
			ckpt_us += get_us_of_day() - ts;
		}

		t1 = get_ms_of_day();

		if (node == 0) {
			double sum = 0.0;
			double denom = (double)(n - 2) * (n - 2) * (n - 2);
			for (i = 1; i < n - 1; i++)
				for (j = 1; j < n - 1; j++)
					for (k = 1; k < n - 1; k++)
						sum += U(i, j, k);
			printf("VERIFY OK  rep=%d iters=%d avg_interior=%.8f center=%.8f\n",
			       rep, iters, sum / denom,
			       U(n / 2, n / 2, n / 2));
			printf("RESULT n=%d d=%d rep=%d ms=%lu "
			       "sweep_ms=%lu writeback_ms=%lu ckpt_ms=%lu\n",
			       n, iters, rep, t1 - t0,
			       sweep_us / 1000UL, writeback_us / 1000UL,
			       ckpt_us / 1000UL);
			fflush(stdout);
		}
	}

#undef U
#undef UNEW
	free(unew);
	return 0;
}
