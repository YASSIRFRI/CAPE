/*
 * cape_papi.h — optional PAPI hardware-counter instrumentation.
 *
 * Purpose: test the hypothesis that CAPE's checkpoint path is memory-bound.
 * The monitor's checkpoint generation walks the dirty set with
 * process_vm_readv + a word-by-word diff against the snapshot — a streaming,
 * cache-hostile access pattern. Wall clock alone cannot separate "slow because
 * of cache/TLB misses" from "slow because of syscalls". PAPI counters can.
 *
 * Build:
 *   make ... PAPI=1                       (auto-detects via pkg-config)
 *   make ... PAPI=1 PAPI_ROOT=/path/papi  (explicit prefix)
 * Without PAPI=1 every function below compiles to nothing — zero cost.
 *
 * Usage (a region accumulates over many start/stop pairs):
 *   cape_papi_init();                     // once per process, before threads
 *   static struct cape_papi_region r = CAPE_PAPI_REGION("generate_ckpt");
 *   struct cape_papi_probe pr;            // stack-local: regions may nest
 *   cape_papi_start(&r, &pr); ...work...; cape_papi_stop(&r, &pr);
 *   cape_papi_report();                   // at exit, prints all regions
 *
 * Counters are per-thread (each thread owns its EventSet) and accumulated
 * atomically into the shared region, so a region measured by N worker threads
 * reports the sum over threads.
 *
 * Event selection: CAPE_PAPI_EVENTS (comma-separated PAPI preset or native
 * names) overrides the default memory-oriented set. Events the CPU cannot
 * count are dropped with a warning instead of failing the run.
 */
#ifndef CAPE_PAPI_H
#define CAPE_PAPI_H

#include <stddef.h>

#define CAPE_PAPI_MAX_EVENTS 12

struct cape_papi_region {
	const char *name;
	long long acc[CAPE_PAPI_MAX_EVENTS];   /* summed over threads/calls */
	unsigned long long ns;                 /* wall ns inside the region */
	unsigned long calls;
	struct cape_papi_region *next;         /* registry chain */
	int registered;
};

#define CAPE_PAPI_REGION(nm) { (nm), { 0 }, 0, 0, NULL, 0 }

/* Live measurement handle. Kept on the caller's stack (not thread-local) so
 * that regions can nest — e.g. a whole-phase region around a worker-lane one. */
struct cape_papi_probe {
	long long snap[CAPE_PAPI_MAX_EVENTS];
	long t0_sec;
	long t0_nsec;                          /* < 0 => probe not started */
};

#ifdef USE_PAPI

#include <papi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default set: loads/stores give the access volume; L1/L2/L3 misses and TLB
 * misses give the memory-stall story; TOT_CYC/TOT_INS normalize it (IPC).
 * PAPI_L3_TCM * 64 B / elapsed = the DRAM bandwidth the phase actually pulls. */
#define CAPE_PAPI_DEFAULT_EVENTS \
	"PAPI_TOT_CYC,PAPI_TOT_INS,PAPI_LD_INS,PAPI_SR_INS," \
	"PAPI_L1_DCM,PAPI_L2_DCM,PAPI_L3_TCM,PAPI_TLB_DM"

static char cape_papi_names[CAPE_PAPI_MAX_EVENTS][PAPI_MAX_STR_LEN];
static int cape_papi_codes[CAPE_PAPI_MAX_EVENTS];
static int cape_papi_nevents;
static int cape_papi_on;
static struct cape_papi_region *cape_papi_regions;
static pthread_mutex_t cape_papi_reg_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Per-thread EventSet: PAPI EventSets are not shareable between threads. */
static __thread int cape_papi_es = PAPI_NULL;

static void cape_papi_register(struct cape_papi_region *r)
{
	if (__atomic_load_n(&r->registered, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_lock(&cape_papi_reg_mtx);
	if (!r->registered) {
		r->next = cape_papi_regions;
		cape_papi_regions = r;
		__atomic_store_n(&r->registered, 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&cape_papi_reg_mtx);
}

/* Build the event list once. Events unsupported on this CPU are dropped so a
 * missing counter (very common for PAPI_L3_TCM / PAPI_TLB_DM) never aborts. */
static void cape_papi_init(void)
{
	const char *spec;
	char buf[1024];
	char *tok, *save = NULL;
	int probe = PAPI_NULL;

	if (cape_papi_on)
		return;
	if (PAPI_library_init(PAPI_VER_CURRENT) != PAPI_VER_CURRENT) {
		fprintf(stderr, "[PAPI] library init failed — counters off\n");
		return;
	}
	if (PAPI_thread_init((unsigned long (*)(void))pthread_self) != PAPI_OK)
		fprintf(stderr, "[PAPI] thread_init failed — per-thread "
				"counters may be wrong\n");

	spec = getenv("CAPE_PAPI_EVENTS");
	if (spec == NULL || spec[0] == '\0')
		spec = CAPE_PAPI_DEFAULT_EVENTS;
	snprintf(buf, sizeof(buf), "%s", spec);

	if (PAPI_create_eventset(&probe) != PAPI_OK) {
		fprintf(stderr, "[PAPI] create_eventset failed — counters off\n");
		return;
	}
	for (tok = strtok_r(buf, ",", &save); tok != NULL;
	     tok = strtok_r(NULL, ",", &save)) {
		int code;

		while (*tok == ' ')
			tok++;
		if (*tok == '\0')
			continue;
		if (cape_papi_nevents >= CAPE_PAPI_MAX_EVENTS) {
			fprintf(stderr, "[PAPI] too many events, ignoring %s\n", tok);
			continue;
		}
		if (PAPI_event_name_to_code(tok, &code) != PAPI_OK ||
		    PAPI_add_event(probe, code) != PAPI_OK) {
			fprintf(stderr, "[PAPI] event unsupported here: %s "
					"(dropped)\n", tok);
			continue;
		}
		cape_papi_codes[cape_papi_nevents] = code;
		snprintf(cape_papi_names[cape_papi_nevents],
			 sizeof(cape_papi_names[0]), "%s", tok);
		cape_papi_nevents++;
	}
	/* Some counters only conflict at start time (limited PMU slots). */
	if (cape_papi_nevents > 0 && PAPI_start(probe) == PAPI_OK) {
		long long dummy[CAPE_PAPI_MAX_EVENTS];
		PAPI_stop(probe, dummy);
	} else if (cape_papi_nevents > 0) {
		fprintf(stderr, "[PAPI] event set could not start (PMU slot "
				"conflict). Reduce CAPE_PAPI_EVENTS.\n");
		cape_papi_nevents = 0;
	}
	PAPI_cleanup_eventset(probe);
	PAPI_destroy_eventset(&probe);

	cape_papi_on = (cape_papi_nevents > 0);
	fprintf(stderr, "[PAPI] %s with %d event(s)\n",
		cape_papi_on ? "enabled" : "disabled", cape_papi_nevents);
}

/* Every thread that measures needs its own started EventSet; cape_papi_start()
 * calls this lazily, so explicit calls are only needed to keep the setup cost
 * out of a measured region. */
static void cape_papi_thread_init(void)
{
	int i;

	if (!cape_papi_on || cape_papi_es != PAPI_NULL)
		return;
	PAPI_register_thread();
	if (PAPI_create_eventset(&cape_papi_es) != PAPI_OK) {
		cape_papi_es = PAPI_NULL;
		return;
	}
	for (i = 0; i < cape_papi_nevents; ++i) {
		if (PAPI_add_event(cape_papi_es, cape_papi_codes[i]) != PAPI_OK) {
			PAPI_destroy_eventset(&cape_papi_es);
			cape_papi_es = PAPI_NULL;
			return;
		}
	}
	if (PAPI_start(cape_papi_es) != PAPI_OK) {
		PAPI_destroy_eventset(&cape_papi_es);
		cape_papi_es = PAPI_NULL;
	}
}

static void cape_papi_start(struct cape_papi_region *r,
			    struct cape_papi_probe *pr)
{
	struct timespec ts;

	pr->t0_sec = 0;
	pr->t0_nsec = -1;                 /* marks "not started" */
	if (!cape_papi_on)
		return;
	if (cape_papi_es == PAPI_NULL)
		cape_papi_thread_init();
	if (cape_papi_es == PAPI_NULL)
		return;
	cape_papi_register(r);
	/* Counters run free; we read deltas so start/stop stays cheap. */
	if (PAPI_read(cape_papi_es, pr->snap) != PAPI_OK)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	pr->t0_sec = ts.tv_sec;
	pr->t0_nsec = ts.tv_nsec;
}

static void cape_papi_stop(struct cape_papi_region *r,
			   struct cape_papi_probe *pr)
{
	long long now[CAPE_PAPI_MAX_EVENTS];
	struct timespec t1;
	int i;

	if (!cape_papi_on || cape_papi_es == PAPI_NULL || pr->t0_nsec < 0)
		return;
	if (PAPI_read(cape_papi_es, now) != PAPI_OK)
		return;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	for (i = 0; i < cape_papi_nevents; ++i)
		__atomic_add_fetch(&r->acc[i], now[i] - pr->snap[i],
				   __ATOMIC_RELAXED);
	__atomic_add_fetch(&r->ns,
			   (unsigned long long)(t1.tv_sec - pr->t0_sec)
				   * 1000000000ULL +
			   (unsigned long long)(t1.tv_nsec - pr->t0_nsec),
			   __ATOMIC_RELAXED);
	__atomic_add_fetch(&r->calls, 1UL, __ATOMIC_RELAXED);
}

/* One line per region per event, plus derived memory-pressure ratios.
 * The "PAPI " prefix keeps the lines greppable out of a job log. */
static void cape_papi_report(void)
{
	struct cape_papi_region *r;
	int i;

	if (!cape_papi_on)
		return;
	for (r = cape_papi_regions; r != NULL; r = r->next) {
		double ms = r->ns / 1e6;
		long long ld = -1, sr = -1, l1 = -1, l3 = -1, cyc = -1, ins = -1;

		fprintf(stderr, "\n[PAPI] region=%s calls=%lu wall_ms=%.3f\n",
			r->name, r->calls, ms);
		for (i = 0; i < cape_papi_nevents; ++i) {
			fprintf(stderr, "  PAPI %-16s %20lld  (%.3f/us)\n",
				cape_papi_names[i], r->acc[i],
				ms > 0 ? r->acc[i] / (ms * 1000.0) : 0.0);
			if (!strcmp(cape_papi_names[i], "PAPI_LD_INS")) ld = r->acc[i];
			if (!strcmp(cape_papi_names[i], "PAPI_SR_INS")) sr = r->acc[i];
			if (!strcmp(cape_papi_names[i], "PAPI_L1_DCM")) l1 = r->acc[i];
			if (!strcmp(cape_papi_names[i], "PAPI_L3_TCM")) l3 = r->acc[i];
			if (!strcmp(cape_papi_names[i], "PAPI_TOT_CYC")) cyc = r->acc[i];
			if (!strcmp(cape_papi_names[i], "PAPI_TOT_INS")) ins = r->acc[i];
		}
		if (cyc > 0 && ins >= 0)
			fprintf(stderr, "  PAPI %-16s %20.4f\n", "IPC",
				(double)ins / (double)cyc);
		if (l1 >= 0 && (ld > 0 || sr > 0))
			fprintf(stderr, "  PAPI %-16s %20.4f\n", "L1_miss/mem_ref",
				(double)l1 / (double)((ld > 0 ? ld : 0) +
						      (sr > 0 ? sr : 0)));
		if (l3 >= 0 && ms > 0)
			/* Each LLC miss pulls one 64 B line from DRAM. */
			fprintf(stderr, "  PAPI %-16s %20.2f MB/s\n",
				"est_dram_bw", (double)l3 * 64.0 / (ms * 1000.0));
	}
	fflush(stderr);
}

#else /* !USE_PAPI — everything compiles away */

static inline void cape_papi_init(void) { }
static inline void cape_papi_thread_init(void) { }
static inline void cape_papi_start(struct cape_papi_region *r,
				   struct cape_papi_probe *pr)
{ (void)r; (void)pr; }
static inline void cape_papi_stop(struct cape_papi_region *r,
				  struct cape_papi_probe *pr)
{ (void)r; (void)pr; }
static inline void cape_papi_report(void) { }

#endif /* USE_PAPI */

#endif /* CAPE_PAPI_H */
