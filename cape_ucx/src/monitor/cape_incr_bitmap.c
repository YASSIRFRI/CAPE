#define _GNU_SOURCE

/*
 *	CAPE Monitor: version 5.0 (UCX)
 *		Using Discontinuous incremental checkpointer.
 *		Using UCX directly to send and receive data between nodes
 *	Some change in Version 5
 * 		- Checkpoint structures ( timespan - 4 bytes, regsiter, memory data)
 * 		- Modified: generate_ckpt, merge_ckpt, merge_extern_ckpt, inject_ckpt
 *
 * Try to implement new model for CAPE
 *
 */
# include <sys/ptrace.h>
# include <sys/syscall.h>
# include <sys/user.h>
# include <sys/wait.h>
# include <assert.h>
# include <stdio.h>
# include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/reg.h>
#include <sys/uio.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <sched.h>
#include <linux/userfaultfd.h>
#include <dirent.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>

#include "../include/cape_monitor.h"
#include "../include/cape_dickpt_uffd.h"
#include "../include/cape_signal.h"

#include <ucp/api/ucp.h>
#ifdef USE_PMIX
#include <pmix.h>
#endif

struct page_node * list_head = NULL, * list_end = NULL;


//list to manage share-data properties that is sent by application
struct shared_data * data_list_head = NULL;
struct shared_data * data_list_tail = NULL;

/* Private-memory masking during checkpoint generation.
 *
 * generate_checkpoint() consults the shared_data whitelist for every word in
 * every dirty page and forces the word_bmp bit to 0 unless the word's
 * address falls inside a registered shared region. This prevents private
 * variables that happen to share a 4 KiB page with shared ones from being
 * shipped to idle workers — a correctness bug when a task body executes on
 * only one worker, since the idle worker's private slots would be clobbered
 * by the executing worker's stack/heap values.
 *
 * Regions are populated by the transpiler via S_START_SHARE_DATA /
 * S_DECLARE_REDUCTION[_REGION], and torn down by S_END_SHARE_DATA at the
 * end of each OpenMP scope (level). If the whitelist is empty, NO words
 * are shipped — this is intentional: an unregistered scope is a transpiler
 * bug we want to surface, not paper over. */
static inline int is_address_shared(unsigned long addr) {
	const struct shared_data *p;
	for (p = data_list_head; p != NULL; p = p->next) {
		unsigned long start = p->addr;
		unsigned long end   = p->addr + (unsigned long)p->len;
		if (addr >= start && addr < end)
			return 1;
	}
	return 0;
}

int process_state = 0; //to follow the state of process
int child_id;
int control_fd = -1;
int userfault_fd = -1;
static int epoll_fd = -1;

struct cape_dickpt_range *tracked_ranges = NULL;
size_t tracked_range_count = 0;
int tracking_is_enabled = 0;

/* ===== bitmap S-section format =====
 *   [BMP_S:8] [n_dirty_pages:4]
 *   [page_addr_0:8] ... [page_addr_{n-1}:8]            // sorted ascending
 *   For each page i:
 *     [word_bmp_i:128]                                  // 1024 bits, 1 = word changed
 *     [changed_words_i: popcount(word_bmp_i) * 4 bytes] // packed in bit-scan order
 *
 * Pack: at checkpoint time we have the pre-image of each faulted page
 * (snapshotted on fault into page_node->data). We process_vm_readv the
 * live post-image, build the word bitmap from a word-by-word diff, and
 * write only the changed post-image words. Pages that faulted but are
 * byte-identical produce a zero bitmap and are skipped entirely.
 *
 * Inject: per page, 1x process_vm_readv (live page) -> patch words
 * where the bit is set from the packed stream -> 1x process_vm_writev.
 * Exactly 1R+1W syscalls per dirty page.
 *
 * Merge: two-pointer over the sorted page_addr arrays. Page only on one
 * side -> emit verbatim. Page on both sides -> emit newer side wholesale
 * (data-parallel programs don't produce real conflicts; the timestamp
 * comparison in merge_external_checkpoint picks newer-wins at the
 * section level). */
#define BMP_S 6
#define BMP_PAGE_SHIFT 12
#define BMP_PAGE_SIZE (1u << BMP_PAGE_SHIFT)
#define BMP_WORDS_PER_PAGE 1024u
#define BMP_WORD_BMP_BYTES 128u

static inline int wbmp_get(const uint8_t *b, unsigned w) {
	return (b[w >> 3] >> (w & 7)) & 1;
}
static inline void wbmp_set(uint8_t *b, unsigned w) {
	b[w >> 3] |= (uint8_t)(1u << (w & 7));
}
static inline unsigned wbmp_popcount(const uint8_t *b) {
	unsigned n = 0, i;
	for (i = 0; i < BMP_WORD_BMP_BYTES; ++i)
		n += (unsigned)__builtin_popcount(b[i]);
	return n;
}



struct bmp_page_view {
	uint64_t       addr;
	const uint8_t *word_bmp;     /* 128 B, into ckpt buffer */
	const uint8_t *changed;      /* n_changed*4 bytes, into ckpt buffer */
	uint32_t       n_changed;
};

struct bmp_section_view {
	int present;
	size_t start;
	size_t end;
	uint32_t n_pages;
	struct bmp_page_view *pages;   /* sorted by addr */
};

static int ckpt_read_mem(const unsigned char *data, size_t size,
			 size_t *pos, void *dst, size_t n)
{
	if (*pos > size || n > size - *pos)
		return 1;
	memcpy(dst, data + *pos, n);
	*pos += n;
	return 0;
}

static int parse_bitmap_section(const unsigned char *data, size_t size,
				size_t payload_pos,
				struct bmp_section_view *view)
{
	size_t pos = payload_pos;
	unsigned long marker;
	uint32_t i;

	memset(view, 0, sizeof(*view));
	view->start = payload_pos;
	view->end = payload_pos;

	if (pos > size || size - pos < sizeof(unsigned long))
		return 0;
	memcpy(&marker, data + pos, sizeof(marker));
	if (marker != BMP_S)
		return 0;
	pos += sizeof(marker);

	view->present = 1;
	if (ckpt_read_mem(data, size, &pos, &view->n_pages,
			  sizeof(view->n_pages)) != 0)
		return 1;

	if (view->n_pages != 0) {
		view->pages = calloc(view->n_pages, sizeof(*view->pages));
		if (view->pages == NULL)
			return 1;
	}

	for (i = 0; i < view->n_pages; ++i) {
		uint64_t addr;
		if (ckpt_read_mem(data, size, &pos, &addr, sizeof(addr)) != 0)
			return 1;
		view->pages[i].addr = addr;
	}

	for (i = 0; i < view->n_pages; ++i) {
		struct bmp_page_view *pg = &view->pages[i];
		unsigned n_changed;
		size_t payload_bytes;

		if (pos > size || size - pos < BMP_WORD_BMP_BYTES)
			return 1;
		pg->word_bmp = data + pos;
		pos += BMP_WORD_BMP_BYTES;

		n_changed = wbmp_popcount(pg->word_bmp);
		if (n_changed > BMP_WORDS_PER_PAGE)
			return 1;
		pg->n_changed = (uint32_t)n_changed;
		payload_bytes = (size_t)n_changed * sizeof(uint32_t);
		if (pos > size || payload_bytes > size - pos)
			return 1;
		pg->changed = data + pos;
		pos += payload_bytes;
	}

	view->end = pos;
	return 0;
}

static void free_bitmap_section(struct bmp_section_view *view)
{
	free(view->pages);
	view->pages = NULL;
	view->n_pages = 0;
	view->present = 0;
}

static int write_bitmap_page_payload(FILE *out, const struct bmp_page_view *pg)
{
	size_t bytes = (size_t)pg->n_changed * sizeof(uint32_t);

	if (fwrite(pg->word_bmp, BMP_WORD_BMP_BYTES, 1, out) != 1)
		return 1;
	if (bytes != 0 && fwrite(pg->changed, bytes, 1, out) != 1)
		return 1;
	return 0;
}

/* ===== reduction lookup / apply =====
 * data_list_head is the per-rank reduction hashmap (linear list, fine
 * for the tiny scalar counts we expect). merge_bitmap_sections checks
 * each dirty word's address; if it's a declared reduction variable,
 * the two sides' values are combined with the variable's op instead
 * of newer-wins. */
static unsigned reduction_size(unsigned char dt)
{
	switch (dt) {
	case CAPE_CHAR: case CAPE_BYTE: case CAPE_UNSIGNED_CHAR:
		return 1;
	case CAPE_SHORT: case CAPE_UNSIGNED_SHORT:
		return 2;
	case CAPE_INT: case CAPE_UNSIGNED_INT: case CAPE_FLOAT:
		return 4;
	case CAPE_LONG: case CAPE_UNSIGNED_LONG: case CAPE_DOUBLE:
	case CAPE_LONG_LONG: case CAPE_LONG_LONG_INT:
		return 8;
	default:
		return 4;
	}
}

static int reduction_matches_addr(const struct shared_data *p, unsigned long addr)
{
	unsigned int elem_size = reduction_size(p->datatype);
	unsigned long start = p->addr;
	unsigned long len = p->len;
	unsigned long end;

	if (elem_size == 0)
		return 0;
	if (len <= elem_size)
		return p->addr == addr;
	if (addr < start)
		return 0;
	end = start + len;
	if (end < start || addr + elem_size > end)
		return 0;
	return ((addr - start) % elem_size) == 0;
}

static struct shared_data *lookup_reduction(unsigned long addr)
{
	struct shared_data *p;
	for (p = data_list_head; p != NULL; p = p->next) {
		if (!reduction_matches_addr(p, addr))
			continue;
		if (p->properties >= D_REDUCTION_SUM &&
		    p->properties <= D_REDUCTION_XOR)
			return p;
	}
	return NULL;
}

#define APPLY_BIN(T, OP) do { T a, b; memcpy(&a, acc, sizeof(T)); \
	memcpy(&b, in, sizeof(T)); a = (T)(a OP b); \
	memcpy(acc, &a, sizeof(T)); } while (0)
#define APPLY_MAX(T) do { T a, b; memcpy(&a, acc, sizeof(T)); \
	memcpy(&b, in, sizeof(T)); if (b > a) memcpy(acc, in, sizeof(T)); \
	} while (0)
#define APPLY_MIN(T) do { T a, b; memcpy(&a, acc, sizeof(T)); \
	memcpy(&b, in, sizeof(T)); if (b < a) memcpy(acc, in, sizeof(T)); \
	} while (0)

static void apply_reduction(unsigned char op, unsigned char dt,
			    void *acc, const void *in)
{
	switch (op) {
	case D_REDUCTION_SUM:
		switch (dt) {
		case CAPE_INT: APPLY_BIN(int32_t, +); break;
		case CAPE_UNSIGNED_INT: APPLY_BIN(uint32_t, +); break;
		case CAPE_LONG: case CAPE_LONG_LONG: case CAPE_LONG_LONG_INT:
			APPLY_BIN(int64_t, +); break;
		case CAPE_UNSIGNED_LONG: APPLY_BIN(uint64_t, +); break;
		case CAPE_FLOAT: APPLY_BIN(float, +); break;
		case CAPE_DOUBLE: APPLY_BIN(double, +); break;
		}
		break;
	case D_REDUCTION_MUL:
		switch (dt) {
		case CAPE_INT: APPLY_BIN(int32_t, *); break;
		case CAPE_UNSIGNED_INT: APPLY_BIN(uint32_t, *); break;
		case CAPE_LONG: case CAPE_LONG_LONG: case CAPE_LONG_LONG_INT:
			APPLY_BIN(int64_t, *); break;
		case CAPE_UNSIGNED_LONG: APPLY_BIN(uint64_t, *); break;
		case CAPE_FLOAT: APPLY_BIN(float, *); break;
		case CAPE_DOUBLE: APPLY_BIN(double, *); break;
		}
		break;
	case D_REDUCTION_MAX:
		switch (dt) {
		case CAPE_INT: APPLY_MAX(int32_t); break;
		case CAPE_UNSIGNED_INT: APPLY_MAX(uint32_t); break;
		case CAPE_LONG: case CAPE_LONG_LONG: case CAPE_LONG_LONG_INT:
			APPLY_MAX(int64_t); break;
		case CAPE_UNSIGNED_LONG: APPLY_MAX(uint64_t); break;
		case CAPE_FLOAT: APPLY_MAX(float); break;
		case CAPE_DOUBLE: APPLY_MAX(double); break;
		}
		break;
	case D_REDUCTION_MIN:
		switch (dt) {
		case CAPE_INT: APPLY_MIN(int32_t); break;
		case CAPE_UNSIGNED_INT: APPLY_MIN(uint32_t); break;
		case CAPE_LONG: case CAPE_LONG_LONG: case CAPE_LONG_LONG_INT:
			APPLY_MIN(int64_t); break;
		case CAPE_UNSIGNED_LONG: APPLY_MIN(uint64_t); break;
		case CAPE_FLOAT: APPLY_MIN(float); break;
		case CAPE_DOUBLE: APPLY_MIN(double); break;
		}
		break;
	case D_REDUCTION_AND: APPLY_BIN(uint64_t, &); break;
	case D_REDUCTION_OR:  APPLY_BIN(uint64_t, |); break;
	case D_REDUCTION_XOR: APPLY_BIN(uint64_t, ^); break;
	}
}

/* Decode a packed bitmap_page_view into dense word/have arrays. */
static void unpack_page(const struct bmp_page_view *pg,
			uint32_t out_words[BMP_WORDS_PER_PAGE],
			uint8_t  out_have[BMP_WORDS_PER_PAGE])
{
	unsigned w, k = 0;
	memset(out_have, 0, BMP_WORDS_PER_PAGE);
	if (pg == NULL)
		return;
	for (w = 0; w < BMP_WORDS_PER_PAGE; ++w) {
		if (wbmp_get(pg->word_bmp, w)) {
			memcpy(&out_words[w],
			       pg->changed + (size_t)k * sizeof(uint32_t),
			       sizeof(uint32_t));
			out_have[w] = 1;
			k++;
		}
	}
}

static int emit_merged_page(FILE *out, uint64_t addr,
			    const struct bmp_page_view *o,
			    const struct bmp_page_view *n)
{
	uint32_t o_words[BMP_WORDS_PER_PAGE], n_words[BMP_WORDS_PER_PAGE];
	uint8_t  o_have[BMP_WORDS_PER_PAGE],  n_have[BMP_WORDS_PER_PAGE];
	uint32_t out_words[BMP_WORDS_PER_PAGE];
	uint8_t  bmp[BMP_WORD_BMP_BYTES];
	unsigned out_count = 0, w = 0;
	size_t bytes;

	unpack_page(o, o_words, o_have);
	unpack_page(n, n_words, n_have);
	memset(bmp, 0, sizeof(bmp));

	while (w < BMP_WORDS_PER_PAGE) {
		int os = o_have[w], ns = n_have[w];
		struct shared_data *r;
		unsigned sz;

		if (!os && !ns) { w++; continue; }

		r = lookup_reduction((unsigned long)addr + (unsigned long)w * 4u);
		sz = (r != NULL) ? reduction_size(r->datatype) : 0;

		if (r != NULL && sz == 8 && w + 1 < BMP_WORDS_PER_PAGE) {
			int o_full = o_have[w] && o_have[w + 1];
			int n_full = n_have[w] && n_have[w + 1];
			uint64_t lo, hi, res;
			uint32_t lo32, hi32;

			if (o_full && n_full) {
				memcpy(&lo, &o_words[w], sizeof(uint64_t));
				memcpy(&hi, &n_words[w], sizeof(uint64_t));
				res = lo;
				apply_reduction(r->properties, r->datatype,
						&res, &hi);
			} else if (o_full) {
				memcpy(&res, &o_words[w], sizeof(uint64_t));
			} else if (n_full) {
				memcpy(&res, &n_words[w], sizeof(uint64_t));
			} else {
				goto per_word;   /* partial coverage; fall back */
			}
			memcpy(&lo32, (uint8_t *)&res, sizeof(uint32_t));
			memcpy(&hi32, (uint8_t *)&res + 4, sizeof(uint32_t));
			wbmp_set(bmp, w);
			wbmp_set(bmp, w + 1);
			out_words[out_count++] = lo32;
			out_words[out_count++] = hi32;
			w += 2;
			continue;
		}
per_word:
		{
			uint32_t v;
			if (r != NULL && os && ns) {
				v = o_words[w];
				apply_reduction(r->properties, r->datatype,
						&v, &n_words[w]);
			} else if (os && ns) {
				v = n_words[w];
			} else if (os) {
				v = o_words[w];
			} else {
				v = n_words[w];
			}
			wbmp_set(bmp, w);
			out_words[out_count++] = v;
		}
		w++;
	}

	if (fwrite(bmp, BMP_WORD_BMP_BYTES, 1, out) != 1)
		return 1;
	bytes = (size_t)out_count * sizeof(uint32_t);
	if (bytes != 0 && fwrite(out_words, bytes, 1, out) != 1)
		return 1;
	return 0;
}

/* Two-pointer walk over sorted page lists. Pages on only one side go
 * out verbatim. Pages on both sides go through emit_merged_page, which
 * does word-level merging with reduction lookup. */
static int merge_bitmap_sections(FILE *out,
				 const struct bmp_section_view *older,
				 const struct bmp_section_view *newer)
{
	unsigned long marker = BMP_S;
	uint32_t total = 0, i;
	uint32_t ia = 0, ib = 0;
	struct merge_slot {
		uint64_t addr;
		const struct bmp_page_view *o;
		const struct bmp_page_view *n;
	} *slots = NULL;
	size_t cap;

	if (!older->present && !newer->present)
		return 0;

	cap = (size_t)older->n_pages + (size_t)newer->n_pages;
	if (cap == 0) {
		if (fwrite(&marker, sizeof(marker), 1, out) != 1 ||
		    fwrite(&total, sizeof(total), 1, out) != 1)
			return 1;
		return 0;
	}
	slots = calloc(cap, sizeof(*slots));
	if (slots == NULL)
		return 1;

	while (ia < older->n_pages || ib < newer->n_pages) {
		if (ib >= newer->n_pages ||
		    (ia < older->n_pages &&
		     older->pages[ia].addr < newer->pages[ib].addr)) {
			slots[total].addr = older->pages[ia].addr;
			slots[total].o = &older->pages[ia++];
			total++;
		} else if (ia >= older->n_pages ||
			   newer->pages[ib].addr < older->pages[ia].addr) {
			slots[total].addr = newer->pages[ib].addr;
			slots[total].n = &newer->pages[ib++];
			total++;
		} else {
			slots[total].addr = newer->pages[ib].addr;
			slots[total].o = &older->pages[ia++];
			slots[total].n = &newer->pages[ib++];
			total++;
		}
	}

	if (fwrite(&marker, sizeof(marker), 1, out) != 1 ||
	    fwrite(&total, sizeof(total), 1, out) != 1)
		goto fail;
	for (i = 0; i < total; ++i) {
		uint64_t a = slots[i].addr;
		if (fwrite(&a, sizeof(a), 1, out) != 1)
			goto fail;
	}
	for (i = 0; i < total; ++i) {
		if (slots[i].o != NULL && slots[i].n != NULL) {
			if (emit_merged_page(out, slots[i].addr,
					     slots[i].o, slots[i].n) != 0)
				goto fail;
		} else {
			const struct bmp_page_view *pg = slots[i].o ?
				slots[i].o : slots[i].n;
			if (write_bitmap_page_payload(out, pg) != 0)
				goto fail;
		}
	}
	free(slots);
	return 0;

fail:
	free(slots);
	return 1;
}

unsigned long child_data_start;

struct user_regs_struct save_regs;
unsigned long node;
int num_nodes;
int ckpt_flag = 0; // to save the state of checkpoint that is received

int current_node=1; //current slave is communicating with master 
int current_job =0; //count the current job
unsigned long number_of_jobs; //save number of step that will be sent from CAPE program
int jobs_per_node; //save the number of step that is divided to a node

/* Master-side dynamic task pool for the legacy DICKPT workshare protocol.
 * Each dispatched checkpoint is one task. A worker finishes the task by
 * generating a checkpoint and sending it back to rank 0; the master receives
 * completions with a UCX wildcard token match and recovers the worker rank
 * from UCX's sender_tag. */
static int *task_idle_workers;
static unsigned char *task_worker_busy;
static int task_idle_count;
static int task_active_workers;
static int task_pool_ready;
static unsigned long task_dispatched_jobs;
static unsigned long task_completed_jobs;
static int cape_task_pool_reset(void);
static void cape_dyn_reset(void);

#define CAPE_TASK_SKIP 0u
#define CAPE_TASK_RUN  1u
/* Dynamic-task directives (master -> worker, TAG_CKPT_DATA opcode byte) */
#define CAPE_TASK_RUN_DYN  2u   /* [fn:8][args_size:8][args][total ckpt] */
#define CAPE_TASK_DONE     3u   /* taskwait satisfied: [total ckpt]      */
#define CAPE_TASK_SHUTDOWN 4u   /* task region over: leave serve loop    */
/* Dynamic-task control ops (worker -> master, TAG_TASK_CTRL opcode byte) */
#define CAPE_TCTL_SUBMIT   1u   /* [fn:8][args_size:8][args][delta ckpt] */
#define CAPE_TCTL_WAIT     2u   /* worker entered taskwait               */
#define CAPE_TCTL_COMPLETE 3u   /* [delta ckpt] of the finished task     */

/* ===== Task dependency DAG (master only) =====
 * The transpiler emits dickpt_task_depend(addr, type) for every depend()
 * item just before a task's dispatch block. The master records them per
 * task (task_pending_*), and at dispatch time derives the predecessor task
 * ids using the standard OpenMP rules:
 *   - IN x      depends on the last writer of x            (RAW)
 *   - OUT/INOUT depends on the last writer and all readers (WAW + WAR)
 * It then blocks dispatch until every predecessor has completed (and thus
 * merged into total_ckpt), so the dependent task's worker — which is shipped
 * total_ckpt as its inject payload — observes its inputs. Tasks with no
 * recorded deps schedule exactly as before. */
#define CAPE_TASK_MAX_DEPS 128

struct cape_dep_item {
	unsigned long addr;
	int type;
};

struct cape_dep_obj {
	unsigned long addr;
	long last_writer;        /* task id of last OUT/INOUT, -1 if none */
	long *readers;           /* task ids of IN's since last writer */
	int n_readers, cap_readers;
	struct cape_dep_obj *next;
};

static struct cape_dep_item task_pending_deps[CAPE_TASK_MAX_DEPS];
static int task_pending_dep_count;
static struct cape_dep_obj *task_dep_objs;   /* per-address dependency state */
static long *task_of_worker;                 /* worker -> running task id, -1 idle */
static unsigned char *task_done;             /* task id -> completed flag */
static size_t task_done_cap;
static long task_next_id;                    /* id of next task to dispatch */

unsigned long timespan = 1 ; // timespan of checkpoints

//checkpoint variables
unsigned char *after_ckpt, *final_ckpt, *total_ckpt, *buffer_ckpt;
FILE *after_ckpt_stream;
FILE *final_ckpt_stream;
FILE *total_ckpt_stream;
size_t after_ckpt_size, final_ckpt_size, total_ckpt_size;
static int final_ckpt_uses_scratch;
int buffer_size;

//Workshare checkpoints
unsigned char *mbefore_ckpt;
FILE *mbefore_ckpt_stream;
size_t mbefore_ckpt_size=0;

int task_ckpt_size=0; //size of a workshare checkpoint

/* Opt-in per-iteration checkpoint-size logging. Enabled by setting the env
 * var CAPE_CKPT_SIZE_LOG=1 (the dedicated benchmark job does this). When on,
 * every app dickpt_generate_ckpt() emits one CKPT_SIZE line with the bytes
 * this rank's delta occupies, so we can watch the dirty set grow as the
 * (e.g. heat-diffusion) front propagates. Off by default — zero overhead. */
static int cape_ckpt_size_log = -1;       /* -1 = not yet resolved */
static long cape_ckpt_size_seq = 0;       /* per-rank generate counter */

static int cape_ckpt_size_log_enabled(void)
{
	if (cape_ckpt_size_log < 0) {
		const char *e = getenv("CAPE_CKPT_SIZE_LOG");
		cape_ckpt_size_log = (e != NULL && e[0] != '\0' && e[0] != '0');
	}
	return cape_ckpt_size_log;
}

//receive buffer
unsigned char *before_buffer;




#if DDEBUG
#define dprintf(fmt, args...) printf(fmt, ## args);
#else
#define dprintf(fmt, args...) do { } while (0)
#endif

#ifdef CAPE_PROFILE
typedef struct {
	/* counters */
	unsigned long process_vm_read_calls, process_vm_write_calls;
	unsigned long process_vm_read_bytes, process_vm_write_bytes;
	unsigned long writeprotect_calls;
	unsigned long dirty_pages_captured;
	unsigned long userfault_events;
	unsigned long waitpid_calls, wait_loops;
	unsigned long poll_calls, poll_timeouts;
	unsigned long ucx_progress_calls;
	unsigned long ucx_progress_active_calls, ucx_progress_idle_calls;
	unsigned long ucx_sendrecv_calls;
	unsigned long ucx_sendrecv_size_calls, ucx_sendrecv_data_calls;
	unsigned long ucx_send_calls, ucx_recv_calls;
	unsigned long ucx_send_bytes, ucx_recv_bytes;
	unsigned long ucx_sendrecv_size_bytes, ucx_sendrecv_data_bytes;
	unsigned long ucx_wait_recv_calls, ucx_wait_send_calls;
	unsigned long ucx_bootstrap_wait_iters;
	unsigned long sigtrap_count;
	unsigned long generate_ckpt_calls;
	unsigned long merge_ext_calls;
	unsigned long allreduce_steps;
	unsigned long tracked_range_count;
	unsigned long generated_ckpt_samples;
	unsigned long merged_ckpt_samples;
	unsigned long long tracked_region_bytes;
	unsigned long long generated_ckpt_last_bytes;
	unsigned long long generated_ckpt_max_bytes;
	unsigned long long generated_ckpt_total_bytes;
	unsigned long long merged_ckpt_last_bytes;
	unsigned long long merged_ckpt_max_bytes;
	unsigned long long merged_ckpt_total_bytes;
	/* ns timers */
	unsigned long long process_vm_read_ns, process_vm_write_ns;
	unsigned long long writeprotect_ns;
	unsigned long long dirty_capture_ns;
	unsigned long long userfault_handle_ns;
	unsigned long long waitpid_ns;
	unsigned long long wait_blocking_ns, wait_for_child_ns, wait_tracked_ns;
	unsigned long long poll_ns;
	unsigned long long ucx_progress_ns;
	unsigned long long ucx_sendrecv_ns;
	unsigned long long ucx_sendrecv_size_ns, ucx_sendrecv_data_ns;
	unsigned long long ucx_send_ns, ucx_recv_ns;
	unsigned long long ucx_wait_ns;
	unsigned long long ucx_wait_recv_ns, ucx_wait_send_ns;
	unsigned long long ucx_bootstrap_wait_ns;
	unsigned long long generate_ckpt_ns;
	unsigned long long merge_ext_ns;
	unsigned long long allreduce_total_ns;
} cape_profile_t;

static cape_profile_t cape_profile;

#define CAPE_PROFILE_NS_VAR(name) struct timespec name
#define CAPE_PROFILE_NS_START(name) clock_gettime(CLOCK_MONOTONIC, &(name))
#define CAPE_PROFILE_INC(field) ((void)(cape_profile.field++))
#define CAPE_PROFILE_ADD(field, value) ((void)(cape_profile.field += (value)))
#define CAPE_PROFILE_SET(field, value) ((void)(cape_profile.field = (value)))
#define CAPE_PROFILE_MAX(field, value) do { \
	unsigned long long _cape_v = (unsigned long long)(value); \
	if (_cape_v > cape_profile.field) cape_profile.field = _cape_v; \
} while (0)
#define CAPE_PROFILE_ADD_NS(field, start) do { \
		struct timespec _now_ts; \
		clock_gettime(CLOCK_MONOTONIC, &_now_ts); \
		cape_profile.field += (unsigned long long)( \
		(_now_ts.tv_sec - (start).tv_sec) * 1000000000ULL \
		+ (_now_ts.tv_nsec - (start).tv_nsec)); \
} while (0)

static unsigned long long cape_profile_elapsed_ns(struct timespec start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)((now.tv_sec - start.tv_sec) * 1000000000ULL
				    + (now.tv_nsec - start.tv_nsec));
}

static void cape_profile_record_generated_ckpt(size_t bytes)
{
	CAPE_PROFILE_SET(generated_ckpt_last_bytes, (unsigned long long)bytes);
	CAPE_PROFILE_MAX(generated_ckpt_max_bytes, bytes);
	CAPE_PROFILE_ADD(generated_ckpt_total_bytes, (unsigned long long)bytes);
	CAPE_PROFILE_INC(generated_ckpt_samples);
}

static void cape_profile_record_merged_ckpt(size_t bytes)
{
	CAPE_PROFILE_SET(merged_ckpt_last_bytes, (unsigned long long)bytes);
	CAPE_PROFILE_MAX(merged_ckpt_max_bytes, bytes);
	CAPE_PROFILE_ADD(merged_ckpt_total_bytes, (unsigned long long)bytes);
	CAPE_PROFILE_INC(merged_ckpt_samples);
}

static double cape_profile_pct_of_tracked(unsigned long long bytes)
{
	if (cape_profile.tracked_region_bytes == 0)
		return 0.0;
	return 100.0 * (double)bytes /
	       (double)cape_profile.tracked_region_bytes;
}

static void cape_profile_report(void)
{
	unsigned long long generated_avg = 0;
	unsigned long long merged_avg = 0;

	if (cape_profile.generated_ckpt_samples != 0)
		generated_avg = cape_profile.generated_ckpt_total_bytes /
				cape_profile.generated_ckpt_samples;
	if (cape_profile.merged_ckpt_samples != 0)
		merged_avg = cape_profile.merged_ckpt_total_bytes /
			     cape_profile.merged_ckpt_samples;

	fprintf(stderr, "\n[DICKPT PROFILE] Node %ld  (ms = ns/1e6)\n", node);
#define P_NS(name) fprintf(stderr, "  %-30s : %10.3f ms\n", #name, cape_profile.name / 1e6)
#define P_CNT(name) fprintf(stderr, "  %-30s : %lu\n", #name, cape_profile.name)
#define P_BYTES(name) fprintf(stderr, "  %-30s : %llu bytes\n", #name, \
		(unsigned long long)cape_profile.name)
	P_NS(wait_for_child_ns); P_NS(wait_blocking_ns); P_NS(wait_tracked_ns);
	P_NS(waitpid_ns); P_CNT(waitpid_calls); P_CNT(wait_loops);
	P_NS(poll_ns); P_CNT(poll_calls); P_CNT(poll_timeouts);
	P_NS(userfault_handle_ns); P_CNT(userfault_events);
	P_CNT(dirty_pages_captured); P_NS(dirty_capture_ns);
	P_NS(writeprotect_ns); P_CNT(writeprotect_calls);
	P_NS(process_vm_read_ns); P_CNT(process_vm_read_calls); P_CNT(process_vm_read_bytes);
	P_NS(process_vm_write_ns); P_CNT(process_vm_write_calls); P_CNT(process_vm_write_bytes);
	P_NS(generate_ckpt_ns); P_CNT(generate_ckpt_calls);
	P_NS(ucx_progress_ns); P_CNT(ucx_progress_calls);
	P_CNT(ucx_progress_active_calls); P_CNT(ucx_progress_idle_calls);
	P_NS(ucx_sendrecv_ns); P_CNT(ucx_sendrecv_calls);
	P_NS(ucx_sendrecv_size_ns); P_CNT(ucx_sendrecv_size_calls); P_CNT(ucx_sendrecv_size_bytes);
	P_NS(ucx_sendrecv_data_ns); P_CNT(ucx_sendrecv_data_calls); P_CNT(ucx_sendrecv_data_bytes);
	P_NS(ucx_send_ns); P_CNT(ucx_send_calls); P_CNT(ucx_send_bytes);
	P_NS(ucx_recv_ns); P_CNT(ucx_recv_calls); P_CNT(ucx_recv_bytes);
	P_NS(ucx_wait_ns);
	P_NS(ucx_wait_recv_ns); P_CNT(ucx_wait_recv_calls);
	P_NS(ucx_wait_send_ns); P_CNT(ucx_wait_send_calls);
	P_NS(merge_ext_ns); P_CNT(merge_ext_calls);
	P_NS(allreduce_total_ns); P_CNT(allreduce_steps);
	P_NS(ucx_bootstrap_wait_ns); P_CNT(ucx_bootstrap_wait_iters);
	P_CNT(sigtrap_count);
	P_CNT(tracked_range_count); P_BYTES(tracked_region_bytes);
	P_CNT(generated_ckpt_samples);
	P_BYTES(generated_ckpt_last_bytes);
	P_BYTES(generated_ckpt_max_bytes);
	fprintf(stderr, "  %-30s : %llu bytes (%.6f%% of tracked)\n",
		"generated_ckpt_avg_bytes",
		(unsigned long long)generated_avg,
		cape_profile_pct_of_tracked(generated_avg));
	P_CNT(merged_ckpt_samples);
	P_BYTES(merged_ckpt_last_bytes);
	P_BYTES(merged_ckpt_max_bytes);
	fprintf(stderr, "  %-30s : %llu bytes (%.6f%% of tracked)\n",
		"merged_ckpt_avg_bytes",
		(unsigned long long)merged_avg,
		cape_profile_pct_of_tracked(merged_avg));
	fprintf(stderr, "  %-30s : %.6f%%\n",
		"generated_last/tracked",
		cape_profile_pct_of_tracked(cape_profile.generated_ckpt_last_bytes));
	fprintf(stderr, "  %-30s : %.6f%%\n",
		"merged_last/tracked",
		cape_profile_pct_of_tracked(cape_profile.merged_ckpt_last_bytes));
	fflush(stderr);
#undef P_NS
#undef P_CNT
#undef P_BYTES
}
#else
#define CAPE_PROFILE_NS_VAR(name)
#define CAPE_PROFILE_NS_START(name) do { } while (0)
#define CAPE_PROFILE_INC(field) do { } while (0)
#define CAPE_PROFILE_ADD(field, value) do { } while (0)
#define CAPE_PROFILE_SET(field, value) do { } while (0)
#define CAPE_PROFILE_MAX(field, value) do { } while (0)
#define CAPE_PROFILE_ADD_NS(field, start) do { } while (0)
static unsigned long long cape_profile_elapsed_ns(struct timespec start)
{
	(void)start;
	return 0;
}
static void cape_profile_record_generated_ckpt(size_t bytes) { (void)bytes; }
static void cape_profile_record_merged_ckpt(size_t bytes) { (void)bytes; }
static inline void cape_profile_report(void) {}
#endif

static FILE *open_binary_memstream(unsigned char **bufloc, size_t *sizeloc)
{
	return open_memstream((char **)bufloc, sizeloc);
}

static int cape_cpuset_count(const cpu_set_t *set)
{
	int cpu, count = 0;

	for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
		if (CPU_ISSET(cpu, set))
			count++;
	}
	return count;
}

static int cape_cpuset_first(const cpu_set_t *set)
{
	int cpu;

	for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
		if (CPU_ISSET(cpu, set))
			return cpu;
	}
	return -1;
}

static int cape_cpuset_nth(const cpu_set_t *set, int n)
{
	int cpu;

	for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
		if (CPU_ISSET(cpu, set)) {
			if (n == 0)
				return cpu;
			n--;
		}
	}
	return -1;
}

static int cape_env_int(const char *name, int fallback)
{
	const char *v = getenv(name);
	char *end = NULL;
	long n;

	if (v == NULL || v[0] == '\0')
		return fallback;
	errno = 0;
	n = strtol(v, &end, 10);
	if (errno != 0 || end == v || n < INT_MIN || n > INT_MAX)
		return fallback;
	return (int)n;
}

static int cape_parse_cpu_list(const char *text, const cpu_set_t *allowed,
			       cpu_set_t *out)
{
	const char *p = text;
	int selected = 0;

	CPU_ZERO(out);
	while (p != NULL && *p != '\0') {
		char *end;
		long first, last;

		while (isspace((unsigned char)*p) || *p == ',')
			p++;
		if (*p == '\0')
			break;

		errno = 0;
		first = strtol(p, &end, 10);
		if (errno != 0 || end == p || first < 0 || first >= CPU_SETSIZE)
			return 1;
		last = first;
		p = end;

		if (*p == '-') {
			p++;
			errno = 0;
			last = strtol(p, &end, 10);
			if (errno != 0 || end == p || last < first ||
			    last >= CPU_SETSIZE)
				return 1;
			p = end;
		}

		for (long cpu = first; cpu <= last; ++cpu) {
			if (CPU_ISSET((int)cpu, allowed)) {
				CPU_SET((int)cpu, out);
				selected++;
			}
		}

		while (isspace((unsigned char)*p))
			p++;
		if (*p != '\0' && *p != ',')
			return 1;
	}

	return selected == 0;
}

static void cape_default_rank_cpuset(const cpu_set_t *allowed, cpu_set_t *out)
{
	int local_rank = cape_env_int("SLURM_LOCALID", -1);
	int local_size = -1;
	int cpus_per_rank = cape_env_int("CAPE_CPUS_PER_RANK", -1);
	int allowed_count = cape_cpuset_count(allowed);

	if (local_rank < 0)
		local_rank = cape_env_int("OMPI_COMM_WORLD_LOCAL_RANK", -1);
	if (local_rank < 0)
		local_rank = cape_env_int("PMI_LOCAL_RANK", -1);
	if (local_rank < 0)
		local_rank = cape_env_int("MV2_COMM_WORLD_LOCAL_RANK", -1);
	if (cpus_per_rank <= 0)
		cpus_per_rank = cape_env_int("SLURM_CPUS_PER_TASK", -1);
	if (cpus_per_rank <= 0)
		local_size = cape_env_int("SLURM_NTASKS_PER_NODE", -1);
	if (cpus_per_rank <= 0 && local_size < 0)
		local_size = cape_env_int("SLURM_TASKS_PER_NODE", -1);
	if (cpus_per_rank <= 0 && local_size < 0)
		local_size = cape_env_int("OMPI_COMM_WORLD_LOCAL_SIZE", -1);
	if (cpus_per_rank <= 0 && local_size < 0)
		local_size = cape_env_int("PMI_LOCAL_SIZE", -1);
	if (cpus_per_rank <= 0 && local_size < 0)
		local_size = cape_env_int("PMIX_LOCAL_SIZE", -1);
	if (cpus_per_rank <= 0 && local_size < 0)
		local_size = cape_env_int("MV2_COMM_WORLD_LOCAL_SIZE", -1);
	if (cpus_per_rank <= 0 && local_size > 0)
		cpus_per_rank = allowed_count / local_size;

	CPU_ZERO(out);
	if (local_rank >= 0 && cpus_per_rank > 0 &&
	    allowed_count >= (local_rank + 1) * cpus_per_rank) {
		int i;
		for (i = 0; i < cpus_per_rank; ++i) {
			int cpu = cape_cpuset_nth(allowed,
						  local_rank * cpus_per_rank + i);
			if (cpu >= 0)
				CPU_SET(cpu, out);
		}
	}

	if (cape_cpuset_count(out) == 0)
		*out = *allowed;
}

static int cape_apply_affinity(int monitor_process)
{
	cpu_set_t allowed, rank_set, target, parsed;
	const char *role = monitor_process ? "monitor" : "child";
	const char *monitor_env;
	const char *child_env;
	int monitor_cpu;

	if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
		perror("sched_getaffinity");
		return 1;
	}

	cape_default_rank_cpuset(&allowed, &rank_set);

	monitor_env = getenv("CAPE_MONITOR_CORE");
	if (monitor_env == NULL || monitor_env[0] == '\0')
		monitor_env = getenv("CAPE_MANAGEMENT_CORE");
	if (monitor_env != NULL && monitor_env[0] != '\0') {
		if (cape_parse_cpu_list(monitor_env,
					&allowed, &parsed) != 0) {
			fprintf(stderr,
				"CAPE affinity: invalid/unsupported monitor CPU list: %s\n",
				monitor_env);
			return 1;
		}
		monitor_cpu = cape_cpuset_first(&parsed);
	} else {
		monitor_cpu = cape_cpuset_first(&rank_set);
	}
	if (monitor_cpu < 0)
		return 1;

	CPU_ZERO(&target);
	if (monitor_process) {
		CPU_SET(monitor_cpu, &target);
	} else {
		child_env = getenv("CAPE_CHILD_CORES");
		if (child_env == NULL || child_env[0] == '\0')
			child_env = getenv("CAPE_COMPUTE_CORES");
		if (child_env == NULL || child_env[0] == '\0')
			child_env = getenv("CAPE_APP_CORES");
		if (child_env != NULL && child_env[0] != '\0') {
			if (cape_parse_cpu_list(child_env, &allowed, &target) != 0) {
				fprintf(stderr,
					"CAPE affinity: invalid/unsupported child CPU list: %s\n",
					child_env);
				return 1;
			}
		} else {
			target = rank_set;
			CPU_CLR(monitor_cpu, &target);
			if (cape_cpuset_count(&target) == 0)
				target = rank_set;
		}
	}

	if (sched_setaffinity(0, sizeof(target), &target) != 0) {
		fprintf(stderr, "CAPE affinity: failed to pin %s: %s\n",
			role, strerror(errno));
		return 1;
	}

	return 0;
}

static int read_remote_memory(pid_t pid, unsigned long remote_addr, void *local_buf,
			      size_t len)
{
	size_t done = 0;
	CAPE_PROFILE_NS_VAR(start_ns);
	CAPE_PROFILE_NS_START(start_ns);

	while (done < len) {
		struct iovec local = {
			.iov_base = (char *)local_buf + done,
			.iov_len = len - done
		};
		struct iovec remote = {
			.iov_base = (void *)(remote_addr + done),
			.iov_len = len - done
		};
		ssize_t rc = process_vm_readv(pid, &local, 1, &remote, 1, 0);

		if (rc <= 0)
			return -1;
		done += (size_t)rc;
	}

	CAPE_PROFILE_ADD_NS(process_vm_read_ns, start_ns);
	CAPE_PROFILE_INC(process_vm_read_calls);
	CAPE_PROFILE_ADD(process_vm_read_bytes, (uint64_t)len);

	return 0;
}

static int write_remote_memory(pid_t pid, const void *local_buf,
			       unsigned long remote_addr, size_t len)
{
	size_t done = 0;
	CAPE_PROFILE_NS_VAR(start_ns);
	CAPE_PROFILE_NS_START(start_ns);

	while (done < len) {
		struct iovec local = {
			.iov_base = (void *)((const char *)local_buf + done),
			.iov_len = len - done
		};
		struct iovec remote = {
			.iov_base = (void *)(remote_addr + done),
			.iov_len = len - done
		};
		ssize_t rc = process_vm_writev(pid, &local, 1, &remote, 1, 0);

		if (rc <= 0)
			return -1;
		done += (size_t)rc;
	}

	CAPE_PROFILE_ADD_NS(process_vm_write_ns, start_ns);
	CAPE_PROFILE_INC(process_vm_write_calls);
	CAPE_PROFILE_ADD(process_vm_write_bytes, (uint64_t)len);

	return 0;
}

static int cape_userfault_writeprotect(unsigned long start, unsigned long len, int enable)
{
	struct uffdio_writeprotect wp;
	CAPE_PROFILE_NS_VAR(start_ns);
	CAPE_PROFILE_NS_START(start_ns);

	if (userfault_fd < 0)
		return -1;

	memset(&wp, 0, sizeof(wp));
	wp.range.start = start;
	wp.range.len = len;
	wp.mode = enable ? UFFDIO_WRITEPROTECT_MODE_WP : 0;

	CAPE_PROFILE_INC(writeprotect_calls);
	if (ioctl(userfault_fd, UFFDIO_WRITEPROTECT, &wp) == -1)
		return -1;
	CAPE_PROFILE_ADD_NS(writeprotect_ns, start_ns);
	return 0;
}

static int cape_capture_dirty_page(unsigned int pid, unsigned long fault_addr)
{
	unsigned long aligned_addr;
	struct page_node *temp_node, *current_node;
	CAPE_PROFILE_NS_VAR(start_ns);
	CAPE_PROFILE_NS_START(start_ns);

	aligned_addr = fault_addr & ~(PAGE_SIZE - 1);
	temp_node = list_head;
	while (temp_node != NULL && temp_node->addr < aligned_addr)
		temp_node = temp_node->next;
	if (temp_node != NULL && temp_node->addr == aligned_addr)
		return 0;

	current_node = malloc(sizeof(struct page_node));
	if (current_node == NULL)
		return 1;
	memset(current_node, 0, sizeof(*current_node));

	if (read_remote_memory(pid, aligned_addr, &(current_node->data), PAGE_SIZE) != 0) {
		free(current_node);
		return 1;
	}

	current_node->addr = aligned_addr;

	if (list_head == NULL) {
		list_head = current_node;
		list_end = current_node;
		CAPE_PROFILE_ADD_NS(dirty_capture_ns, start_ns);
		CAPE_PROFILE_INC(dirty_pages_captured);
		return 0;
	}

	if (aligned_addr > list_end->addr) {
		current_node->before = list_end;
		list_end->next = current_node;
		list_end = current_node;
		CAPE_PROFILE_ADD_NS(dirty_capture_ns, start_ns);
		CAPE_PROFILE_INC(dirty_pages_captured);
		return 0;
	}

	if (aligned_addr < list_head->addr) {
		current_node->next = list_head;
		list_head->before = current_node;
		list_head = current_node;
		CAPE_PROFILE_ADD_NS(dirty_capture_ns, start_ns);
		CAPE_PROFILE_INC(dirty_pages_captured);
		return 0;
	}

	current_node->next = temp_node;
	current_node->before = temp_node->before;
	temp_node->before->next = current_node;
	temp_node->before = current_node;
	CAPE_PROFILE_ADD_NS(dirty_capture_ns, start_ns);
	CAPE_PROFILE_INC(dirty_pages_captured);
	return 0;
}

static int cape_handle_userfault_event(void)
{
	struct uffd_msg msg;
	ssize_t nread;
	unsigned long page_addr;
	CAPE_PROFILE_NS_VAR(start_ns);
	CAPE_PROFILE_NS_START(start_ns);

	nread = read(userfault_fd, &msg, sizeof(msg));
	if (nread == -1) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		perror("read(userfaultfd)");
		return -1;
	}
	if (nread == 0)
		return -1;
	if ((size_t)nread < sizeof(msg))
		return -1;
	if (msg.event != UFFD_EVENT_PAGEFAULT)
		return 0;
	if ((msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) == 0)
		return 0;

	CAPE_PROFILE_INC(userfault_events);
	page_addr = msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
	if (cape_capture_dirty_page(child_id, page_addr) != 0) {
		fprintf(stderr, "Monitor %ld: failed to snapshot page 0x%lx\n",
		        node, page_addr);
		return -1;
	}
	if (cape_userfault_writeprotect(page_addr, PAGE_SIZE, 0) == -1) {
		perror("ioctl(UFFDIO_WRITEPROTECT clear)");
		return -1;
	}

	CAPE_PROFILE_ADD_NS(userfault_handle_ns, start_ns);
	return 0;
}

int cape_drain_userfaultfd(void)
{
	struct epoll_event ev;

	if (userfault_fd < 0 || !tracking_is_enabled)
		return 0;

	while (epoll_wait(epoll_fd, &ev, 1, 0) > 0) {
		if ((ev.events & EPOLLIN) == 0)
			break;
		if (cape_handle_userfault_event() != 0)
			return -1;
	}

	return 0;
}

int cape_wait_for_child_event(pid_t pid, int *status)
{
	CAPE_PROFILE_NS_VAR(total_start_ns);
	CAPE_PROFILE_NS_START(total_start_ns);

	if (userfault_fd < 0 || !tracking_is_enabled)
	{
		CAPE_PROFILE_NS_VAR(wait_start_ns);
		pid_t rc;
		CAPE_PROFILE_NS_START(wait_start_ns);
		/* Retry on EINTR — otherwise any signal delivered to the monitor
		 * before tracking is enabled (e.g. between fork and the app's
		 * first int3) causes us to return -1, the main loop to break,
		 * and control_fd[0] to close while the app is mid-sendmsg →
		 * "sendmsg(userfaultfd setup) failed: Broken pipe". */
		do {
			rc = waitpid(pid, status, 0);
		} while (rc == -1 && errno == EINTR);

		CAPE_PROFILE_ADD_NS(wait_for_child_ns, wait_start_ns);
		CAPE_PROFILE_ADD_NS(wait_blocking_ns, wait_start_ns);
		CAPE_PROFILE_ADD_NS(waitpid_ns, wait_start_ns);
		CAPE_PROFILE_INC(waitpid_calls);
		return rc;
	}

	for (;;) {
		CAPE_PROFILE_NS_VAR(waitpid_start_ns);
		pid_t rc;

		CAPE_PROFILE_INC(wait_loops);
		CAPE_PROFILE_NS_START(waitpid_start_ns);
		rc = waitpid(pid, status, WNOHANG);
		CAPE_PROFILE_ADD_NS(waitpid_ns, waitpid_start_ns);
		CAPE_PROFILE_INC(waitpid_calls);

		if (rc == pid) {
			CAPE_PROFILE_ADD_NS(wait_for_child_ns, total_start_ns);
			CAPE_PROFILE_ADD_NS(wait_tracked_ns, total_start_ns);
			return rc;
		}
		if (rc == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (cape_drain_userfaultfd() != 0)
			return -1;

		{
			struct epoll_event evs[2];
			int nfds, i;
			CAPE_PROFILE_NS_VAR(poll_start_ns);
			/* Poll userfaultfd briefly, then loop back to waitpid(WNOHANG)
			 * to detect ptrace stops from the child. */
			int timeout = 1;

			CAPE_PROFILE_NS_START(poll_start_ns);
			if (epoll_fd < 0) {
				/* userfaultfd setup hasn't happened yet (app is still
				 * starting up and about to sendmsg its uffd to us).
				 * Just sleep briefly and loop back to waitpid — do NOT
				 * touch epoll_fd here or we'd EBADF-out, exit, and
				 * close the control socket while the app is mid-sendmsg. */
				struct timespec ts = { 0, 1 * 1000 * 1000 }; /* 1 ms */
				nanosleep(&ts, NULL);
				nfds = 0;
			} else {
				nfds = epoll_wait(epoll_fd, evs, 2, timeout);
			}
			CAPE_PROFILE_ADD_NS(poll_ns, poll_start_ns);
			CAPE_PROFILE_INC(poll_calls);
			if (nfds == 0)
				CAPE_PROFILE_INC(poll_timeouts);
			if (nfds == -1 && errno != EINTR)
				return -1;
			for (i = 0; i < nfds; i++) {
				if (evs[i].data.fd == userfault_fd &&
				    (evs[i].events & EPOLLIN) != 0) {
					if (cape_handle_userfault_event() != 0)
						return -1;
				}
			}
		}
	}
}

int cape_receive_userfaultfd_setup(void)
{
	char payload[sizeof(struct cape_dickpt_ctl_header) +
		     (CAPE_DICKPT_MAX_RANGES * sizeof(struct cape_dickpt_range))];
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} control;
	struct cape_dickpt_ctl_header *header;
	struct cape_dickpt_range *ranges;
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	ssize_t rc;

	if (userfault_fd >= 0)
		return 0;

	memset(&msg, 0, sizeof(msg));
	memset(&control, 0, sizeof(control));
	memset(payload, 0, sizeof(payload));

	iov.iov_base = payload;
	iov.iov_len = sizeof(payload);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.buf;
	msg.msg_controllen = sizeof(control.buf);

	rc = recvmsg(control_fd, &msg, 0);
	if (rc == -1) {
		perror("recvmsg(userfaultfd setup)");
		return 1;
	}
	if ((size_t)rc < sizeof(*header))
		return 1;

	header = (struct cape_dickpt_ctl_header *)payload;
	if (header->type != CAPE_DICKPT_CTL_UFFD_SETUP)
		return 1;
	if (header->count > CAPE_DICKPT_MAX_RANGES)
		return 1;
	if ((size_t)rc < sizeof(*header) +
			 (header->count * sizeof(struct cape_dickpt_range)))
		return 1;

	free(tracked_ranges);
	tracked_ranges = NULL;
	tracked_range_count = 0;
	CAPE_PROFILE_SET(tracked_range_count, 0);
	CAPE_PROFILE_SET(tracked_region_bytes, 0);

	if (header->count > 0) {
		unsigned long long tracked_bytes = 0;
		size_t i;

		tracked_ranges = malloc(header->count * sizeof(*tracked_ranges));
		if (tracked_ranges == NULL)
			return 1;

		ranges = (struct cape_dickpt_range *)(payload + sizeof(*header));
		memcpy(tracked_ranges, ranges, header->count * sizeof(*tracked_ranges));
		tracked_range_count = header->count;
		for (i = 0; i < tracked_range_count; i++)
			tracked_bytes += tracked_ranges[i].len;
		CAPE_PROFILE_SET(tracked_range_count, (unsigned long)tracked_range_count);
		CAPE_PROFILE_SET(tracked_region_bytes, tracked_bytes);
	}

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
			memcpy(&userfault_fd, CMSG_DATA(cmsg), sizeof(userfault_fd));
			break;
		}
	}

	if (userfault_fd < 0)
		return 1;

	/* Create epoll instance and register userfault_fd */
	epoll_fd = epoll_create1(0);
	if (epoll_fd < 0) {
		perror("epoll_create1");
		return 1;
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = userfault_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, userfault_fd, &ev) < 0) {
		perror("epoll_ctl(userfault_fd)");
		return 1;
	}

	return 0;
}

static int cape_writeprotect_tracked_ranges(int enable)
{
	size_t i;

	for (i = 0; i < tracked_range_count; ++i) {
		if (cape_userfault_writeprotect(tracked_ranges[i].start,
						tracked_ranges[i].len,
						enable) == -1) {
			perror("ioctl(UFFDIO_WRITEPROTECT range)");
			return 1;
		}
	}

	return 0;
}

/* =========================================================================
 * UCX State and Helpers
 * ========================================================================= */
#ifdef USE_PMIX
static pmix_proc_t   pmix_myproc;
#endif
static ucp_context_h ucp_context;
static ucp_worker_h  ucp_worker;
static ucp_ep_h     *ucp_endpoints;  /* one per peer process */

static unsigned char *ucx_scratch_send;
static unsigned char *ucx_scratch_recv;
static unsigned char *ucx_scratch_merge;
static unsigned char *ucx_scratch_total_in;
static ucp_mem_h ucx_scratch_send_memh;
static ucp_mem_h ucx_scratch_recv_memh;
static ucp_mem_h ucx_scratch_merge_memh;
static ucp_mem_h ucx_scratch_total_in_memh;
static size_t ucx_scratch_send_cap;
static size_t ucx_scratch_recv_cap;
static size_t ucx_scratch_merge_cap;
static size_t ucx_scratch_total_in_cap;
static size_t ucx_scratch_initial_cap;
static int total_ckpt_uses_scratch;

/* Verbose per-event tracing. Off by default — it does an fprintf+fflush on
 * every SIGTRAP and every UCX wait, which dominates the runtime and serialises
 * the monitor. Build with -DCAPE_DEBUG to re-enable for debugging. */
#ifdef CAPE_DEBUG
#define CAPE_DBG(fmt, args...) do { \
	fprintf(stderr, "CAPE_DBG rank=%ld pid=%d: " fmt "\n", \
		node, getpid(), ## args); \
	fflush(stderr); \
} while (0)
#else
#define CAPE_DBG(fmt, args...) do { } while (0)
#endif

/* Per-request state allocated by UCX in the request headroom */
typedef struct {
    volatile int completed;
    ucs_status_t status;
    size_t       recv_len;
    ucp_tag_t    sender_tag;
} cape_ucx_req_t;

static void cape_ucx_req_init(void *request)
{
    cape_ucx_req_t *r = (cape_ucx_req_t *)request;
    r->completed  = 0;
    r->status     = UCS_OK;
    r->recv_len   = 0;
    r->sender_tag = 0;
}

static void cape_send_cb(void *request, ucs_status_t status, void *user_data)
{
    cape_ucx_req_t *r = (cape_ucx_req_t *)request;
    r->status    = status;
    r->completed = 1;
}

static void cape_recv_cb(void *request, ucs_status_t status,
                         const ucp_tag_recv_info_t *info, void *user_data)
{
    cape_ucx_req_t *r = (cape_ucx_req_t *)request;
    r->status    = status;
    if (info != NULL) {
        r->recv_len   = info->length;
        r->sender_tag = info->sender_tag;
    }
    r->completed = 1;
}

static size_t cape_parse_size_env(const char *name, size_t fallback)
{
    const char *v = getenv(name);
    char *end = NULL;
    unsigned long long n;

    if (v == NULL || v[0] == '\0')
        return fallback;
    errno = 0;
    n = strtoull(v, &end, 0);
    if (errno != 0 || end == v || n == 0 || n > (unsigned long long)SIZE_MAX)
        return fallback;
    return (size_t)n;
}

static int cape_scratch_reserve(unsigned char **buf, size_t *cap, size_t need)
{
    if (need == 0)
        need = 1;
    if (*cap >= need)
        return 0;

    fprintf(stderr,
            "CAPE UCX: scratch buffer too small (need=%zu cap=%zu); "
            "increase CAPE_UCX_SCRATCH_BYTES\n",
            need, *cap);
    return 1;
}

static int cape_copy_to_scratch(unsigned char **buf, size_t *cap,
                                const void *src, size_t len)
{
    if (cape_scratch_reserve(buf, cap, len) != 0)
        return 1;
    if (len != 0)
        memcpy(*buf, src, len);
    return 0;
}

static int cape_ucx_scratch_init(void)
{
    const size_t default_scratch = 256ull * 1024ull * 1024ull;

    ucx_scratch_initial_cap = cape_parse_size_env("CAPE_UCX_SCRATCH_BYTES",
                                                  default_scratch);
    if (posix_memalign((void **)&ucx_scratch_send, 4096,
                       ucx_scratch_initial_cap) != 0 ||
        posix_memalign((void **)&ucx_scratch_recv, 4096,
                       ucx_scratch_initial_cap) != 0 ||
        posix_memalign((void **)&ucx_scratch_merge, 4096,
                       ucx_scratch_initial_cap) != 0 ||
        posix_memalign((void **)&ucx_scratch_total_in, 4096,
                       ucx_scratch_initial_cap) != 0) {
        fprintf(stderr,
                "CAPE UCX: failed to allocate four %zu-byte scratch buffers\n",
                ucx_scratch_initial_cap);
        return 1;
    }
    ucx_scratch_send_cap = ucx_scratch_recv_cap = ucx_scratch_merge_cap =
        ucx_scratch_total_in_cap = ucx_scratch_initial_cap;

    {
        ucp_mem_map_params_t mp;
        ucs_status_t st;

        memset(&mp, 0, sizeof(mp));
        mp.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                        UCP_MEM_MAP_PARAM_FIELD_LENGTH |
                        UCP_MEM_MAP_PARAM_FIELD_FLAGS |
                        UCP_MEM_MAP_PARAM_FIELD_MEMORY_TYPE;
        mp.length = ucx_scratch_initial_cap;
        mp.flags = UCP_MEM_MAP_NONBLOCK;
        mp.memory_type = UCS_MEMORY_TYPE_HOST;

#define CAPE_MAP_SCRATCH(ptr, memh, name) do { \
        mp.address = (ptr); \
        st = ucp_mem_map(ucp_context, &mp, &(memh)); \
        if (st != UCS_OK) { \
            fprintf(stderr, "CAPE UCX: ucp_mem_map(%s, %zu bytes) failed: %s\n", \
                    (name), ucx_scratch_initial_cap, ucs_status_string(st)); \
            return 1; \
        } \
    } while (0)

        CAPE_MAP_SCRATCH(ucx_scratch_send, ucx_scratch_send_memh, "send");
        CAPE_MAP_SCRATCH(ucx_scratch_recv, ucx_scratch_recv_memh, "recv");
        CAPE_MAP_SCRATCH(ucx_scratch_merge, ucx_scratch_merge_memh, "merge");
        CAPE_MAP_SCRATCH(ucx_scratch_total_in, ucx_scratch_total_in_memh, "total_in");
#undef CAPE_MAP_SCRATCH
    }
    return 0;
}

static void cape_ucx_scratch_cleanup(void)
{
    if (ucx_scratch_send_memh != NULL)
        ucp_mem_unmap(ucp_context, ucx_scratch_send_memh);
    if (ucx_scratch_recv_memh != NULL)
        ucp_mem_unmap(ucp_context, ucx_scratch_recv_memh);
    if (ucx_scratch_merge_memh != NULL)
        ucp_mem_unmap(ucp_context, ucx_scratch_merge_memh);
    if (ucx_scratch_total_in_memh != NULL)
        ucp_mem_unmap(ucp_context, ucx_scratch_total_in_memh);
    ucx_scratch_send_memh = ucx_scratch_recv_memh = NULL;
    ucx_scratch_merge_memh = ucx_scratch_total_in_memh = NULL;
    free(ucx_scratch_send);
    free(ucx_scratch_recv);
    free(ucx_scratch_merge);
    free(ucx_scratch_total_in);
    ucx_scratch_send = ucx_scratch_recv = ucx_scratch_merge = NULL;
    ucx_scratch_total_in = NULL;
    ucx_scratch_send_cap = ucx_scratch_recv_cap = ucx_scratch_merge_cap = 0;
    ucx_scratch_total_in_cap = 0;
}

static int cape_total_stream_update_size(void)
{
    long pos;

    if (total_ckpt_stream == NULL)
        return 0;
    if (fflush(total_ckpt_stream) != 0)
        return 1;
    pos = ftell(total_ckpt_stream);
    if (pos < 0)
        return 1;
    total_ckpt_size = (size_t)pos;
    return 0;
}

static ucp_mem_h cape_scratch_memh_for(const void *ptr)
{
    const unsigned char *p = ptr;

    if (ptr == NULL)
        return NULL;
    if (ucx_scratch_send != NULL &&
        p >= ucx_scratch_send && p < ucx_scratch_send + ucx_scratch_send_cap)
        return ucx_scratch_send_memh;
    if (ucx_scratch_recv != NULL &&
        p >= ucx_scratch_recv && p < ucx_scratch_recv + ucx_scratch_recv_cap)
        return ucx_scratch_recv_memh;
    if (ucx_scratch_merge != NULL &&
        p >= ucx_scratch_merge && p < ucx_scratch_merge + ucx_scratch_merge_cap)
        return ucx_scratch_merge_memh;
    if (ucx_scratch_total_in != NULL &&
        p >= ucx_scratch_total_in &&
        p < ucx_scratch_total_in + ucx_scratch_total_in_cap)
        return ucx_scratch_total_in_memh;
    return NULL;
}

static int cape_total_scratch_buffer(const unsigned char *avoid,
                                     unsigned char **buf,
                                     size_t *cap)
{
    if (avoid == ucx_scratch_merge) {
        *buf = ucx_scratch_total_in;
        *cap = ucx_scratch_total_in_cap;
    } else {
        *buf = ucx_scratch_merge;
        *cap = ucx_scratch_merge_cap;
    }
    return 0;
}

static FILE *cape_total_open_scratch_stream(size_t need)
{
    FILE *stream;
    unsigned char *buf;
    size_t cap;

    if (need == SIZE_MAX)
        return NULL;
    cape_total_scratch_buffer(total_ckpt, &buf, &cap);
    if (cape_scratch_reserve(&buf, &cap, need + 1) != 0)
        return NULL;
    stream = fmemopen(buf, cap, "wb");
    if (stream == NULL)
        return NULL;
    total_ckpt = buf;
    total_ckpt_size = 0;
    total_ckpt_uses_scratch = 1;
    total_ckpt_stream = stream;
    return stream;
}

static FILE *cape_final_open_scratch_stream(unsigned char **bufloc,
                                            size_t *sizeloc)
{
    FILE *stream;

    stream = fmemopen(ucx_scratch_merge, ucx_scratch_merge_cap, "wb");
    if (stream == NULL)
        return NULL;
    *bufloc = ucx_scratch_merge;
    *sizeloc = 0;
    final_ckpt_uses_scratch = 1;
    return stream;
}

static void cape_total_release(void)
{
    if (total_ckpt_stream != NULL) {
        fclose(total_ckpt_stream);
        total_ckpt_stream = NULL;
    }
    if (total_ckpt != NULL && !total_ckpt_uses_scratch)
        free(total_ckpt);
    total_ckpt = NULL;
    total_ckpt_size = 0;
    total_ckpt_uses_scratch = 0;
}

static unsigned long long cape_ucx_wait(void *req, size_t expect_len,
					int check_len, ucp_tag_t *out_tag)
{
	CAPE_PROFILE_NS_VAR(wait_start_ns);
	CAPE_PROFILE_NS_START(wait_start_ns);

    if (out_tag) *out_tag = 0;
    if (req == NULL)
        return 0;
    if (UCS_PTR_IS_ERR(req)) {
        fprintf(stderr, "CAPE UCX error: %s\n",
                ucs_status_string(UCS_PTR_STATUS(req)));
        exit(1);
    }
    cape_ucx_req_t *r = (cape_ucx_req_t *)req;
    unsigned idle_iters = 0;
    CAPE_DBG("ucx_wait begin expect_len=%zu check_len=%d", expect_len, check_len);
    while (!r->completed) {
        unsigned events = ucp_worker_progress(ucp_worker);
        CAPE_PROFILE_INC(ucx_progress_calls);
        if (events == 0 && ++idle_iters >= 10000000) {
            CAPE_DBG("ucx_wait still waiting expect_len=%zu check_len=%d", expect_len, check_len);
            idle_iters = 0;
        } else if (events != 0) {
            idle_iters = 0;
        }
    }
    CAPE_DBG("ucx_wait done status=%s recv_len=%zu sender_tag=0x%lx",
             ucs_status_string(r->status), r->recv_len,
             (unsigned long)r->sender_tag);
#ifdef CAPE_PROFILE
    unsigned long long wait_ns = cape_profile_elapsed_ns(wait_start_ns);
    CAPE_PROFILE_ADD(ucx_wait_ns, wait_ns);
    CAPE_PROFILE_ADD(ucx_progress_ns, wait_ns);
#else
    unsigned long long wait_ns = 0;
#endif
    if (out_tag)
        *out_tag = r->sender_tag;
    if (r->status != UCS_OK) {
        fprintf(stderr, "CAPE UCX request failed: %s\n",
                ucs_status_string(r->status));
        ucp_request_free(req);
        exit(1);
    }
    if (check_len && (r->recv_len != 0) && (r->recv_len != expect_len)) {
        fprintf(stderr, "CAPE UCX recv length mismatch: got=%zu expected=%zu\n",
                r->recv_len, expect_len);
        r->completed  = 0;
        r->status     = UCS_OK;
        r->recv_len   = 0;
        r->sender_tag = 0;
        ucp_request_free(req);
        exit(1);
    }
    r->completed  = 0;
    r->status     = UCS_OK;
    r->recv_len   = 0;
    r->sender_tag = 0;
    ucp_request_free(req);
    return wait_ns;
}

#define CAPE_UCX_TAG(sender_rank, token) \
	((uint64_t)((((uint32_t)(sender_rank) & 0x0fffU) << 20) | ((uint32_t)(token) & 0x000fffffU)))
#define CAPE_UCX_TAG_MASK        ((uint64_t)0x00000000ffffffffULL)
#define CAPE_UCX_TAG_TOKEN_MASK  ((uint64_t)0x00000000000fffffULL)

static int cape_ucx_sender_from_tag(ucp_tag_t tag)
{
	return (int)(((uint64_t)tag >> 20) & 0x0fffU);
}

static ucp_tag_message_h cape_ucx_probe_msg(ucp_tag_t recv_tag,
                                            ucp_tag_t recv_mask,
                                            ucp_tag_recv_info_t *info,
                                            unsigned long long *wait_ns_out)
{
    ucp_tag_message_h msg;
    unsigned idle_iters = 0;
    CAPE_PROFILE_NS_VAR(wait_start_ns);
    CAPE_PROFILE_NS_START(wait_start_ns);

	for (;;) {
		msg = ucp_tag_probe_nb(ucp_worker, recv_tag, recv_mask,
	                               1, info);
        if (msg != NULL)
            break;

        {
            unsigned events = ucp_worker_progress(ucp_worker);
            CAPE_PROFILE_INC(ucx_progress_calls);
            if (events == 0) {
                CAPE_PROFILE_INC(ucx_progress_idle_calls);
                if (++idle_iters >= 1000000) {
                    CAPE_DBG("ucx_probe waiting tag=0x%lx mask=0x%lx",
                             (unsigned long)recv_tag,
                             (unsigned long)recv_mask);
                    sched_yield();
                    idle_iters = 0;
                }
            } else {
                CAPE_PROFILE_INC(ucx_progress_active_calls);
                idle_iters = 0;
            }
        }
    }

#ifdef CAPE_PROFILE
    *wait_ns_out = cape_profile_elapsed_ns(wait_start_ns);
    CAPE_PROFILE_ADD(ucx_wait_ns, *wait_ns_out);
    CAPE_PROFILE_ADD(ucx_progress_ns, *wait_ns_out);
#else
    *wait_ns_out = 0;
#endif
	return msg;
}

static ucp_tag_message_h cape_ucx_probe_msg_exact(ucp_tag_t recv_tag,
                                                  ucp_tag_recv_info_t *info,
                                                  unsigned long long *wait_ns_out)
{
	return cape_ucx_probe_msg(recv_tag, CAPE_UCX_TAG_MASK, info,
	                          wait_ns_out);
}

static ucp_tag_message_h cape_ucx_probe_msg_any(uint32_t token,
                                                ucp_tag_recv_info_t *info,
                                                unsigned long long *wait_ns_out)
{
	return cape_ucx_probe_msg(CAPE_UCX_TAG(0, token),
	                          CAPE_UCX_TAG_TOKEN_MASK, info,
	                          wait_ns_out);
}

static size_t cape_ucx_sendrecv_probe(
        const void *sendbuf, size_t sendlen, int dest,
        void       *recvbuf, size_t recvcap, int src,
        uint32_t    token)
{
    CAPE_PROFILE_NS_VAR(start_ns);
    CAPE_PROFILE_NS_START(start_ns);
    ucp_tag_t send_tag = CAPE_UCX_TAG(node, token);
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src,  token);
    ucp_tag_recv_info_t info;
    ucp_tag_message_h msg;
    ucp_mem_h send_memh = cape_scratch_memh_for(sendbuf);
    ucp_mem_h recv_memh = cape_scratch_memh_for(recvbuf);
    unsigned long long probe_wait_ns = 0;
    unsigned long long recv_wait_ns;
    unsigned long long send_wait_ns;

    ucp_request_param_t sp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = cape_send_cb
    };
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                      | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };

    if (send_memh != NULL) {
        sp.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMH;
        sp.memh = send_memh;
    }
    if (recv_memh != NULL) {
        rp.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMH;
        rp.memh = recv_memh;
    }

	void *sreq = ucp_tag_send_nbx(ucp_endpoints[dest], sendbuf, sendlen,
	                                  send_tag, &sp);
	if (UCS_PTR_IS_ERR(sreq)) {
		fprintf(stderr, "CAPE UCX send failed before probe: %s\n",
			ucs_status_string(UCS_PTR_STATUS(sreq)));
		exit(1);
	}
	msg = cape_ucx_probe_msg_exact(recv_tag, &info, &probe_wait_ns);
    if (info.length > recvcap) {
        fprintf(stderr,
                "CAPE UCX: probed message too large (len=%zu cap=%zu); "
                "increase CAPE_UCX_SCRATCH_BYTES\n",
                info.length, recvcap);
        exit(1);
    }

    void *rreq = ucp_tag_msg_recv_nbx(ucp_worker, recvbuf, info.length,
                                      msg, &rp);
    recv_wait_ns = cape_ucx_wait(rreq, info.length, 1, NULL);
    send_wait_ns = cape_ucx_wait(sreq, 0, 0, NULL);
#ifdef CAPE_PROFILE
    {
        unsigned long long sendrecv_ns = cape_profile_elapsed_ns(start_ns);
        CAPE_PROFILE_ADD(ucx_sendrecv_ns, sendrecv_ns);
        CAPE_PROFILE_INC(ucx_sendrecv_calls);
        CAPE_PROFILE_ADD(ucx_send_bytes, (uint64_t)sendlen);
        CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)info.length);
        CAPE_PROFILE_ADD(ucx_wait_recv_ns, probe_wait_ns + recv_wait_ns);
        CAPE_PROFILE_ADD(ucx_wait_send_ns, send_wait_ns);
        CAPE_PROFILE_INC(ucx_wait_recv_calls);
        CAPE_PROFILE_INC(ucx_wait_send_calls);
        CAPE_PROFILE_ADD(ucx_sendrecv_data_ns, sendrecv_ns);
        CAPE_PROFILE_INC(ucx_sendrecv_data_calls);
        CAPE_PROFILE_ADD(ucx_sendrecv_data_bytes, (uint64_t)sendlen);
    }
#endif
    return info.length;
}

static unsigned char *cape_ucx_recv_probe_alloc(size_t *recvlen, int src,
                                                uint32_t token)
{
    CAPE_PROFILE_NS_VAR(start_ns);
    CAPE_PROFILE_NS_START(start_ns);
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src, token);
    ucp_tag_recv_info_t info;
    ucp_tag_message_h msg;
    unsigned long long probe_wait_ns = 0;
    unsigned long long recv_wait_ns;
    unsigned char *buf;
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                      | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };

	CAPE_DBG("recv_probe begin src=%d token=0x%x", src, token);
	msg = cape_ucx_probe_msg_exact(recv_tag, &info, &probe_wait_ns);
	CAPE_DBG("recv_probe matched src=%d token=0x%x len=%zu sender_tag=0x%lx",
		 src, token, info.length, (unsigned long)info.sender_tag);
    buf = malloc(info.length == 0 ? 1 : info.length);
    if (buf == NULL) {
        perror("malloc(probed UCX recv)");
        exit(1);
    }

    void *req = ucp_tag_msg_recv_nbx(ucp_worker, buf, info.length, msg, &rp);
    recv_wait_ns = cape_ucx_wait(req, info.length, 1, NULL);
#ifdef CAPE_PROFILE
    CAPE_PROFILE_ADD_NS(ucx_recv_ns, start_ns);
    CAPE_PROFILE_INC(ucx_recv_calls);
    CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)info.length);
    CAPE_PROFILE_ADD(ucx_wait_recv_ns, probe_wait_ns + recv_wait_ns);
    CAPE_PROFILE_INC(ucx_wait_recv_calls);
#endif
	*recvlen = info.length;
	CAPE_DBG("recv_probe done src=%d token=0x%x len=%zu", src, token, *recvlen);
	return buf;
}

static unsigned char *cape_ucx_recv_probe_alloc_any(size_t *recvlen,
                                                    int *sender,
                                                    uint32_t token)
{
	CAPE_PROFILE_NS_VAR(start_ns);
	CAPE_PROFILE_NS_START(start_ns);
	ucp_tag_recv_info_t info;
	ucp_tag_message_h msg;
	unsigned long long probe_wait_ns = 0;
	unsigned long long recv_wait_ns;
	unsigned char *buf;
	ucp_request_param_t rp = {
	    .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
	                  | UCP_OP_ATTR_FIELD_FLAGS,
	    .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
	    .cb.recv      = cape_recv_cb
	};

	CAPE_DBG("recv_probe_any begin token=0x%x", token);
	msg = cape_ucx_probe_msg_any(token, &info, &probe_wait_ns);
	CAPE_DBG("recv_probe_any matched token=0x%x len=%zu sender=%d sender_tag=0x%lx",
		 token, info.length, cape_ucx_sender_from_tag(info.sender_tag),
		 (unsigned long)info.sender_tag);
	buf = malloc(info.length == 0 ? 1 : info.length);
	if (buf == NULL) {
		perror("malloc(probed wildcard UCX recv)");
		exit(1);
	}

	void *req = ucp_tag_msg_recv_nbx(ucp_worker, buf, info.length, msg, &rp);
	recv_wait_ns = cape_ucx_wait(req, info.length, 1, NULL);
#ifdef CAPE_PROFILE
	CAPE_PROFILE_ADD_NS(ucx_recv_ns, start_ns);
	CAPE_PROFILE_INC(ucx_recv_calls);
	CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)info.length);
	CAPE_PROFILE_ADD(ucx_wait_recv_ns, probe_wait_ns + recv_wait_ns);
	CAPE_PROFILE_INC(ucx_wait_recv_calls);
#endif
	*recvlen = info.length;
	if (sender != NULL)
		*sender = cape_ucx_sender_from_tag(info.sender_tag);
	CAPE_DBG("recv_probe_any done token=0x%x len=%zu sender=%d",
		 token, *recvlen, sender ? *sender : -1);
	return buf;
}

/* Receive a message already matched by ucp_tag_probe_nb into a fresh
 * malloc'd buffer. Companion to the non-blocking probe used by the dynamic
 * task scheduler's event pump. */
static unsigned char *cape_ucx_recv_matched_alloc(ucp_tag_message_h msg,
						  const ucp_tag_recv_info_t *info,
						  size_t *recvlen, int *sender)
{
	unsigned char *buf;
	ucp_request_param_t rp = {
	    .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
	                  | UCP_OP_ATTR_FIELD_FLAGS,
	    .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
	    .cb.recv      = cape_recv_cb
	};

	buf = malloc(info->length == 0 ? 1 : info->length);
	if (buf == NULL) {
		perror("malloc(matched UCX recv)");
		exit(1);
	}
	{
		void *req = ucp_tag_msg_recv_nbx(ucp_worker, buf, info->length,
						 msg, &rp);
		cape_ucx_wait(req, info->length, 1, NULL);
	}
	*recvlen = info->length;
	if (sender != NULL)
		*sender = cape_ucx_sender_from_tag(info->sender_tag);
	return buf;
}

/* Simple blocking send via UCX tag. */
static void cape_ucx_send(const void *buf, size_t len, int dest, uint32_t token)
{
    CAPE_PROFILE_NS_VAR(start_ns);
    CAPE_PROFILE_NS_START(start_ns);
    ucp_tag_t tag = CAPE_UCX_TAG(node, token);
    ucp_request_param_t sp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = cape_send_cb
    };
    ucp_mem_h memh = cape_scratch_memh_for(buf);
    if (memh != NULL) {
        sp.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMH;
        sp.memh = memh;
    }
    CAPE_DBG("send begin dest=%d token=0x%x len=%zu", dest, token, len);
    void *req = ucp_tag_send_nbx(ucp_endpoints[dest], buf, len, tag, &sp);
    unsigned long long send_wait_ns = cape_ucx_wait(req, 0, 0, NULL);
    CAPE_DBG("send done dest=%d token=0x%x len=%zu", dest, token, len);
    CAPE_PROFILE_ADD_NS(ucx_send_ns, start_ns);
    CAPE_PROFILE_INC(ucx_send_calls);
    CAPE_PROFILE_ADD(ucx_send_bytes, (uint64_t)len);
    CAPE_PROFILE_ADD(ucx_wait_send_ns, send_wait_ns);
    CAPE_PROFILE_INC(ucx_wait_send_calls);
}

/* Simple blocking recv via UCX tag. */
static void cape_ucx_recv(void *buf, size_t len, int src, uint32_t token)
{
    CAPE_PROFILE_NS_VAR(start_ns);
    CAPE_PROFILE_NS_START(start_ns);
    ucp_tag_t tag = CAPE_UCX_TAG(src, token);
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                      | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };
    ucp_mem_h memh = cape_scratch_memh_for(buf);
    if (memh != NULL) {
        rp.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMH;
        rp.memh = memh;
    }
    CAPE_DBG("recv begin src=%d token=0x%x len=%zu", src, token, len);
    void *req = ucp_tag_recv_nbx(ucp_worker, buf, len, tag, CAPE_UCX_TAG_MASK, &rp);
    unsigned long long recv_wait_ns = cape_ucx_wait(req, len, 1, NULL);
    CAPE_DBG("recv done src=%d token=0x%x len=%zu", src, token, len);
    CAPE_PROFILE_ADD_NS(ucx_recv_ns, start_ns);
    CAPE_PROFILE_INC(ucx_recv_calls);
    CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)len);
    CAPE_PROFILE_ADD(ucx_wait_recv_ns, recv_wait_ns);
    CAPE_PROFILE_INC(ucx_wait_recv_calls);
}

/* Tag tokens for different message types to avoid collisions */
#define TAG_CKPT_DATA     0x01
#define TAG_CKPT_FLAG     0x02
#define TAG_BCAST_SIZE    0x03
#define TAG_BCAST_DATA    0x04
#define TAG_SCATTER_SIZE  0x05
#define TAG_SCATTER_DATA  0x06
/* Worker -> master control stream for dynamic tasks (SUBMIT/WAIT/COMPLETE).
 * One tag so messages from the same worker stay FIFO: a task's COMPLETE is
 * always processed after the SUBMITs it issued, and before the WAIT the
 * worker sends when it re-enters taskwait. */
#define TAG_TASK_CTRL     0x07
/* critical/atomic checkpoint chain: rank k -> k+1 state hand-off, and the
 * last rank's broadcast of the final section state to everyone else. */
#define TAG_CRIT_CHAIN    0x08
#define TAG_CRIT_FINAL    0x09
#define TAG_ALLREDUCE_BASE 0x100

static const char *cape_ucx_bootstrap_id(void)
{
    /* Cached so callers can hold the returned pointer for the lifetime of
     * the process. Built as SLURM_JOB_ID + "_" + SLURM_STEP_ID so concurrent
     * srun steps inside one allocation (e.g. 4 parallel reps from the
     * benchmark script) get distinct rendezvous files. */
    static char buf[64];
    static int cached = 0;
    const char *override;

    if (cached)
        return buf;

    override = getenv("CAPE_UCX_BOOTSTRAP_ID");
    if (override && *override) {
        snprintf(buf, sizeof(buf), "%s", override);
    } else {
        const char *job  = getenv("SLURM_JOB_ID");
        const char *step = getenv("SLURM_STEP_ID");
        if (!job  || !*job)  job  = "local";
        if (!step || !*step) step = "0";
        snprintf(buf, sizeof(buf), "%s_%s", job, step);
    }
    cached = 1;
    return buf;
}

static const char *cape_ucx_bootstrap_dir(void)
{
    const char *sharedir = getenv("CAPE_UCX_BOOTSTRAP_DIR");
    if (!sharedir || !*sharedir)
        sharedir = getenv("SLURM_SUBMIT_DIR");
    if (!sharedir || !*sharedir)
        sharedir = ".";
    return sharedir;
}

#ifndef USE_PMIX
/* NFS-safe publish: after creating a file under `dir`, fsync the parent
 * directory's fd so the new entry is committed to the NFS server and its
 * mtime bumps — this lets other clients' cache revalidation detect the
 * change instead of waiting out their negative-lookup cache (~30s).
 * `filepath` is the full path of the file just created. */
static void fs_publish_dir_entry(const char *dir, const char *filepath)
{
    int ffd = open(filepath, O_RDONLY);
    if (ffd >= 0) { fsync(ffd); close(ffd); }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
}

/* NFS-safe existence probe for `basename` inside `dir`. Forces a fresh
 * READDIR RPC by opening the directory each time; negative dentry cache on
 * `access()` can otherwise block for acregmin/acdirmin (~30s by default). */
static int fs_entry_exists(const char *dir, const char *basename)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, basename) == 0) { found = 1; break; }
    }
    closedir(d);
    return found;
}

/* Exchange addresses via shared filesystem (same approach as cape.c) */
static void ucx_exchange_addresses_via_fs(const char *dir, const char *jobid,
                                          ucp_address_t *local_addr,
                                          size_t local_addr_len)
{
    char path[512];
    char rdy_base[128];
    FILE *f;

    /* Remove stale rdy file from previous runs before writing anything */
    snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%ld", dir, jobid, node);
    unlink(path);

    snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%ld", dir, jobid, node);
    f = fopen(path, "wb");
    if (!f) { perror("CAPE UCX: write addr file"); exit(1); }
    if (fwrite(&local_addr_len, sizeof(size_t), 1, f) != 1) {
        perror("CAPE UCX: write addr length");
        fclose(f);
        exit(1);
    }
    if (fwrite(local_addr, 1, local_addr_len, f) != local_addr_len) {
        perror("CAPE UCX: write addr payload");
        fclose(f);
        exit(1);
    }
    fclose(f);
    fs_publish_dir_entry(dir, path);

    /* Phase 1: write addr file, then signal ready */
    snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%ld", dir, jobid, node);
    f = fopen(path, "w");
    if (!f) { perror("CAPE UCX: write rdy file"); exit(1); }
    fclose(f);
    fs_publish_dir_entry(dir, path);

    /* Go marker is a separate file (not a rewrite of rdy_0) to avoid NFS
     * page-cache staleness on its content — readers see it via a fresh
     * READDIR in fs_entry_exists instead of a cached read(). */
    char go_base[128];
    snprintf(go_base, sizeof(go_base), "cape_ucx_%s_go", jobid);

    if (node == 0) {
        /* Clear any stale go marker from a previous run before waiting. */
        snprintf(path, sizeof(path), "%s/%s", dir, go_base);
        unlink(path);
        fs_publish_dir_entry(dir, path);
    }

    CAPE_PROFILE_NS_VAR(bootstrap_wait_start_ns);
    CAPE_PROFILE_NS_START(bootstrap_wait_start_ns);
    if (node == 0) {
        /* Master waits for all workers' addr+rdy files */
        for (int i = 1; i < num_nodes; i++) {
            snprintf(rdy_base, sizeof(rdy_base), "cape_ucx_%s_rdy_%d", jobid, i);
            while (!fs_entry_exists(dir, rdy_base)) {
                CAPE_PROFILE_INC(ucx_bootstrap_wait_iters);
                usleep(10000);
            }
        }
        /* Publish the go marker as a brand-new file. */
        snprintf(path, sizeof(path), "%s/%s", dir, go_base);
        f = fopen(path, "w");
        if (!f) { perror("CAPE UCX: create go file"); exit(1); }
        fclose(f);
        fs_publish_dir_entry(dir, path);
    } else {
        /* Workers wait for the go marker to appear via a fresh READDIR. */
        while (!fs_entry_exists(dir, go_base)) {
            CAPE_PROFILE_INC(ucx_bootstrap_wait_iters);
            usleep(10000);
        }
    }
    CAPE_PROFILE_ADD_NS(ucx_bootstrap_wait_ns, bootstrap_wait_start_ns);

    ucs_status_t st;
    ucp_endpoints = malloc(num_nodes * sizeof(ucp_ep_h));
    for (int i = 0; i < num_nodes; i++) {
        snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%d", dir, jobid, i);
        f = fopen(path, "rb");
        if (!f) { perror("CAPE UCX: read peer addr file"); exit(1); }
        size_t peer_len;
        if (fread(&peer_len, sizeof(size_t), 1, f) != 1) {
            fprintf(stderr, "CAPE UCX: failed to read peer addr length from %s\n", path);
            fclose(f);
            exit(1);
        }
        if (peer_len == 0 || peer_len > (1U << 20)) {
            fprintf(stderr, "CAPE UCX: invalid peer addr length %zu in %s\n", peer_len, path);
            fclose(f);
            exit(1);
        }
        ucp_address_t *peer_addr = malloc(peer_len);
        if (fread(peer_addr, 1, peer_len, f) != peer_len) {
            fprintf(stderr, "CAPE UCX: failed to read peer addr payload from %s\n", path);
            free(peer_addr);
            fclose(f);
            exit(1);
        }
        fclose(f);

        ucp_ep_params_t ep_params;
        memset(&ep_params, 0, sizeof(ep_params));
        ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
        ep_params.address    = peer_addr;
        st = ucp_ep_create(ucp_worker, &ep_params, &ucp_endpoints[i]);
        free(peer_addr);
        if (st != UCS_OK) {
            fprintf(stderr, "CAPE UCX: ucp_ep_create(%d) failed: %s\n",
                    i, ucs_status_string(st));
            exit(1);
        }
    }
}
#endif

/* Initialize UCX context, worker, and endpoints */
static void cape_ucx_init(void)
{
    CAPE_DBG("cape_ucx_init enter");
    /* 1. Get rank and size */
#ifdef USE_PMIX
    CAPE_DBG("PMIx_Init begin");
    PMIX_PROC_CONSTRUCT(&pmix_myproc);
    pmix_status_t pst = PMIx_Init(&pmix_myproc, NULL, 0);
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Init failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    node = (unsigned long)pmix_myproc.rank;
    CAPE_DBG("PMIx_Init done nspace=%s rank=%ld", pmix_myproc.nspace, node);

    {
        int step_tasks = cape_env_int("SLURM_STEP_NUM_TASKS", -1);
        if (step_tasks <= 0)
            step_tasks = cape_env_int("PMI_SIZE", -1);
        if (step_tasks <= 0)
            step_tasks = cape_env_int("PMIX_SIZE", -1);
        if (step_tasks <= 0)
            step_tasks = cape_env_int("SLURM_NTASKS", -1);
        if (step_tasks > 0) {
            num_nodes = step_tasks;
        } else {
            pmix_proc_t wildcard;
            pmix_value_t *val;

            PMIX_PROC_CONSTRUCT(&wildcard);
            PMIX_LOAD_NSPACE(wildcard.nspace, pmix_myproc.nspace);
            wildcard.rank = PMIX_RANK_WILDCARD;

            pst = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val);
            if (pst != PMIX_SUCCESS) {
                fprintf(stderr, "CAPE UCX: PMIx_Get(PMIX_JOB_SIZE) failed: %s\n",
                        PMIx_Error_string(pst));
                exit(1);
            }
            num_nodes = (int)val->data.uint32;
            PMIX_VALUE_RELEASE(val);
        }
    }
    CAPE_DBG("rank/size resolved num_nodes=%d env SLURM_STEP_NUM_TASKS=%s SLURM_NTASKS=%s PMI_SIZE=%s PMIX_SIZE=%s",
             num_nodes,
             getenv("SLURM_STEP_NUM_TASKS") ? getenv("SLURM_STEP_NUM_TASKS") : "",
             getenv("SLURM_NTASKS") ? getenv("SLURM_NTASKS") : "",
             getenv("PMI_SIZE") ? getenv("PMI_SIZE") : "",
             getenv("PMIX_SIZE") ? getenv("PMIX_SIZE") : "");
    if ((int)node < 0 || (int)node >= num_nodes) {
        fprintf(stderr,
                "CAPE UCX: PMIx rank %ld is outside step size %d\n",
                node, num_nodes);
        exit(1);
    }
#else
    const char *rank_str = getenv("SLURM_PROCID");
    const char *size_str = getenv("SLURM_NTASKS");
    if (!rank_str) rank_str = getenv("OMPI_COMM_WORLD_RANK");
    if (!size_str) size_str = getenv("OMPI_COMM_WORLD_SIZE");
    if (!rank_str || !size_str) {
        fprintf(stderr, "CAPE UCX: cannot determine rank/size. "
                "Run via srun (sets SLURM_PROCID/SLURM_NTASKS) "
                "or compile with -DUSE_PMIX.\n");
        exit(1);
    }
    node      = (unsigned long)atoi(rank_str);
    num_nodes = atoi(size_str);
    CAPE_DBG("rank/size resolved via env num_nodes=%d", num_nodes);
#endif

    /* 2. Initialise UCX context */
    CAPE_DBG("ucp_init begin");
    ucp_params_t ucp_params;
    memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES
                            | UCP_PARAM_FIELD_REQUEST_SIZE
                            | UCP_PARAM_FIELD_REQUEST_INIT;
    /* Tag-matching only — WAKEUP would force ucp_worker_wait/epoll
     * blocking semantics in cape_ucx_wait, which adds ~1ms latency per
     * progress iteration and kills bandwidth. We busy-spin progress,
     * matching what cape.c does. */
    ucp_params.features     = UCP_FEATURE_TAG;
    ucp_params.request_size = sizeof(cape_ucx_req_t);
    ucp_params.request_init = cape_ucx_req_init;

    ucp_config_t *config;
    ucp_config_read(NULL, NULL, &config);
    ucs_status_t st = ucp_init(&ucp_params, config, &ucp_context);
    ucp_config_release(config);
    if (st != UCS_OK) {
        fprintf(stderr, "CAPE UCX: ucp_init failed: %s\n",
                ucs_status_string(st));
        exit(1);
    }
    CAPE_DBG("ucp_init done");

    /* 3. Create a single-threaded UCX worker */
    CAPE_DBG("ucp_worker_create begin");
    ucp_worker_params_t wp;
    memset(&wp, 0, sizeof(wp));
    wp.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    wp.thread_mode = UCS_THREAD_MODE_SINGLE;
    st = ucp_worker_create(ucp_context, &wp, &ucp_worker);
    if (st != UCS_OK) {
        fprintf(stderr, "CAPE UCX: ucp_worker_create failed: %s\n",
                ucs_status_string(st));
        exit(1);
    }
    CAPE_DBG("ucp_worker_create done");
    if (cape_ucx_scratch_init() != 0)
        exit(1);
    CAPE_DBG("scratch init done cap=%zu", ucx_scratch_initial_cap);

    /* 4. Exchange worker addresses and build endpoints */
    CAPE_DBG("ucp_worker_get_address begin");
    ucp_address_t *local_addr;
    size_t         local_addr_len;
    ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_len);
    CAPE_DBG("ucp_worker_get_address done len=%zu", local_addr_len);

#ifdef USE_PMIX
    char pmix_key[64];
    snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%ld", node);

    pmix_value_t kval;
    PMIX_VALUE_CONSTRUCT(&kval);
    kval.type          = PMIX_BYTE_OBJECT;
    kval.data.bo.bytes = (char *)local_addr;
    kval.data.bo.size  = local_addr_len;

    CAPE_DBG("PMIx_Put begin key=%s len=%zu", pmix_key, local_addr_len);
    pst = PMIx_Put(PMIX_GLOBAL, pmix_key, &kval);
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Put failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    CAPE_DBG("PMIx_Put done");
    CAPE_DBG("PMIx_Commit begin");
    pst = PMIx_Commit();
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Commit failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    CAPE_DBG("PMIx_Commit done");

    pmix_info_t fence_info;
    PMIX_INFO_CONSTRUCT(&fence_info);
    PMIX_INFO_LOAD(&fence_info, PMIX_COLLECT_DATA, NULL, PMIX_BOOL);
    CAPE_DBG("PMIx_Fence begin");
    pst = PMIx_Fence(NULL, 0, &fence_info, 1);
    PMIX_INFO_DESTRUCT(&fence_info);
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Fence failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    CAPE_DBG("PMIx_Fence done; peer PMIx_Get begins");

    ucp_endpoints = malloc(num_nodes * sizeof(ucp_ep_h));
    for (int i = 0; i < num_nodes; i++) {
        pmix_proc_t peer;
        PMIX_PROC_CONSTRUCT(&peer);
        PMIX_LOAD_NSPACE(peer.nspace, pmix_myproc.nspace);
        peer.rank = (pmix_rank_t)i;

        snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%d", i);
        pmix_value_t *peer_val;
        CAPE_DBG("PMIx_Get begin peer=%d key=%s", i, pmix_key);
        pst = PMIx_Get(&peer, pmix_key, NULL, 0, &peer_val);
        if (pst != PMIX_SUCCESS) {
            fprintf(stderr, "CAPE UCX: PMIx_Get(addr, rank=%d) failed: %s\n",
                    i, PMIx_Error_string(pst));
            exit(1);
        }
        CAPE_DBG("PMIx_Get done peer=%d len=%zu", i, peer_val->data.bo.size);
        ucp_ep_params_t ep_params;
        memset(&ep_params, 0, sizeof(ep_params));
        ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
        ep_params.address    = (ucp_address_t *)peer_val->data.bo.bytes;
        CAPE_DBG("ucp_ep_create begin peer=%d", i);
        st = ucp_ep_create(ucp_worker, &ep_params, &ucp_endpoints[i]);
        PMIX_VALUE_RELEASE(peer_val);
        if (st != UCS_OK) {
            fprintf(stderr, "CAPE UCX: ucp_ep_create(%d) failed: %s\n",
                    i, ucs_status_string(st));
            exit(1);
        }
        CAPE_DBG("ucp_ep_create done peer=%d", i);
    }
#else
    const char *jobid    = cape_ucx_bootstrap_id();
    const char *sharedir = cape_ucx_bootstrap_dir();
    CAPE_DBG("fs bootstrap begin dir=%s id=%s", sharedir, jobid);
    ucx_exchange_addresses_via_fs(sharedir, jobid, local_addr, local_addr_len);
    CAPE_DBG("fs bootstrap done");
#endif

    ucp_worker_release_address(ucp_worker, local_addr);
    CAPE_DBG("cape_ucx_init done");
}

/* Finalize UCX (close endpoints, destroy worker, cleanup context) */
static void cape_ucx_finalize(void)
{
    ucp_request_param_t close_param;
    memset(&close_param, 0, sizeof(close_param));
    close_param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
    close_param.flags        = UCP_EP_CLOSE_FLAG_FORCE;
    for (int i = 0; i < num_nodes; i++) {
        void *req = ucp_ep_close_nbx(ucp_endpoints[i], &close_param);
        if (UCS_PTR_IS_ERR(req))
            continue;
        if (req == NULL)
            continue;
        while (ucp_request_check_status(req) == UCS_INPROGRESS)
            ucp_worker_progress(ucp_worker);
        ucp_request_free(req);
    }
    free(ucp_endpoints);
    ucp_endpoints = NULL;

    cape_ucx_scratch_cleanup();
    ucp_worker_destroy(ucp_worker);
    ucp_cleanup(ucp_context);

#ifdef USE_PMIX
    PMIx_Finalize(NULL, 0);
#endif
    /* Non-PMIx bootstrap files are cleaned up by the launch script
       (rm -rf bootstrap_dir).  Do NOT unlink them here — other
       monitors may still be reading them during their exchange. */
}

/* ---------------------------------------------------------------------------
 * Prototypes
 * ---------------------------------------------------------------------------
 */
int clear_list(struct page_node *list);
int send_int_value_to_child(int value);
int get_long_int_from_child(unsigned long *value);
int init_jobs_per_node();
int ioctl_read_data(unsigned int pid, unsigned long src, void *dst, int len);
void tracer_wait ( pid_t pid, int * status, int options, struct user * u );
int lock_process_memory(unsigned int pid);
int unlock_process_memory(unsigned int pid);
int require_generate_checkpoint();
int require_send_checkpoint();
int require_receive_checkpoint();
int require_dispatch_task_checkpoint();
int require_receive_task_checkpoint();
int require_task_spawn(unsigned long desc_addr);
int require_task_serve(unsigned long desc_addr);
int require_task_wait(unsigned long desc_addr);
int require_task_complete(void);
int require_task_region_end(void);
int require_critical_enter(void);
int require_critical_exit(void);
int require_inject_checkpoint();
int require_waitfor_checkpoint();
int require_broadcast_checkpoint();
int read_shared_data();
void end_shared_data();
int require_allreduce_checkpoint();
int merge_external_checkpoint(FILE *src_ckpt_stream,
			      unsigned char *src_ckpt_data,
			      size_t src_ckpt_size);
int cape_receive_userfaultfd_setup(void);
int cape_wait_for_child_event(pid_t pid, int *status);
int cape_drain_userfaultfd(void);

/* ==========================================================================
 * Monitor process
 * 1. Fork another process to run CAPE program
 * 2. Wait for CAPE program finish the start-up
 * 3. Open driver file
 * 4. Monitor CAPE program
 * 	If SIGSEGV (Page-fault occur): Save page addr, data and set page to writable
 * 	If SIGTRAP (Debug signal): do the job that are required by CAPE program
 * ==========================================================================
 */
int main(int argc, char * argv[]){
	char * exec_file; 
	int status, rc;
	struct user u ;
	struct user_regs_struct regs;
	int control_pair[2];
	char control_fd_text[32];

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <app> [app-args...]\n", argv[0]);
		return 1;
	}

	exec_file = argv[1];
	CAPE_DBG("monitor main start app=%s argc=%d", exec_file, argc);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control_pair) == -1) {
		perror("socketpair");
		return 1;
	}


	switch ( child_id = fork ( ) ) {
		case -1 :
			perror ( "fork" ) ;
			return 1 ;
		case 0 :	
			close(control_pair[0]);
			fprintf(stderr, "CAPE_DBG child pid=%d: before exec app=%s\n", getpid(), exec_file);
			fflush(stderr);
			snprintf(control_fd_text, sizeof(control_fd_text), "%d", control_pair[1]);
			setenv(CAPE_DICKPT_ENV_SOCK_FD, control_fd_text, 1);
			if (cape_apply_affinity(0) != 0)
				return 2;
			{
				//aslr disable
				int _p = personality(0xffffffff);
				if (_p >= 0 && !(_p & ADDR_NO_RANDOMIZE))
					personality((unsigned long)_p | ADDR_NO_RANDOMIZE);
			}
			/* PMIx/SLURM marks the launched process non-dumpable to
			 * keep credentials out of core dumps; the child inherits
			 * that across fork() and the kernel then refuses
			 * PTRACE_TRACEME with EPERM. Re-mark ourselves dumpable
			 * before asking to be traced. */
			if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) {
				fprintf(stderr,
					"CAPE_DBG child pid=%d: PR_SET_DUMPABLE failed: %s\n",
					getpid(), strerror(errno));
				fflush(stderr);
			}
			if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) {
				int saved_errno = errno;
				char scope_buf[16] = "?";
				char tracer_buf[64] = "?";
				int fd = open("/proc/sys/kernel/yama/ptrace_scope", O_RDONLY);
				if (fd >= 0) {
					ssize_t n = read(fd, scope_buf, sizeof(scope_buf) - 1);
					if (n > 0) scope_buf[n] = 0;
					close(fd);
				}
				fd = open("/proc/self/status", O_RDONLY);
				if (fd >= 0) {
					char buf[4096];
					ssize_t n = read(fd, buf, sizeof(buf) - 1);
					if (n > 0) {
						buf[n] = 0;
						char *p = strstr(buf, "TracerPid:");
						if (p) {
							char *e = strchr(p, '\n');
							if (e) *e = 0;
							snprintf(tracer_buf, sizeof(tracer_buf), "%s", p);
						}
					}
					close(fd);
				}
				fprintf(stderr,
					"CAPE_DBG child pid=%d: PTRACE_TRACEME FAILED: %s "
					"(yama.ptrace_scope=%s %s) "
					"-- if scope>=2 the cluster admin must lower it, "
					"or run with CAP_SYS_PTRACE; if TracerPid is nonzero, "
					"slurmstepd is already attached\n",
					getpid(), strerror(saved_errno), scope_buf, tracer_buf);
				fflush(stderr);
				_exit(3);
			}
			execv ( exec_file , &argv[1] );
			perror (exec_file) ;
			_exit(2);
		default :
			control_fd = control_pair[0];
			close(control_pair[1]);
			CAPE_DBG("parent after fork child_pid=%d", child_id);
			if (cape_apply_affinity(1) != 0)
				return 1;
			CAPE_DBG("parent before cape_ucx_init");
			cape_ucx_init();
			CAPE_DBG("parent after cape_ucx_init");
			break;
	}
	
	

	//wait for cape program finish startup
	CAPE_DBG("waiting for child exec startup");
	for ( process_state = 0 ; process_state != 2 ; ) {
		tracer_wait ( child_id, & status, 0, & u ) ;
		CAPE_DBG("startup wait state=%d status=0x%x orig_rax=%lld",
			 process_state, status, (long long)u.regs.orig_rax);
		if (WIFEXITED(status)) {
			fprintf(stderr,
				"CAPE monitor: child exited during startup (code=%d) before reaching exec sync\n",
				WEXITSTATUS(status));
			exit(1);
		}
		if (WIFSIGNALED(status)) {
			fprintf(stderr,
				"CAPE monitor: child killed during startup by signal %d%s (status=0x%x)\n",
				WTERMSIG(status),
				WCOREDUMP(status) ? " (core dumped)" : "",
				status);
			exit(1);
		}
		switch ( process_state ) {
		case 0 :	//Wait for ``execve'' to start
			if ( u . regs . orig_rax == SYS_execve )
				process_state = 1 ;
			break ;
		case 1 :	//Wait for ``execve'' to finish and set the breakpoint
			if ( u . regs . orig_rax != SYS_execve )
				process_state = 2 ;
			break ;
		case 2 :	//Everything is set up : go !
			break ;
		default :
			assert ( 0 ) ;
		}//end switch
	}//end for

	CAPE_DBG("continuing child after startup");
	ptrace(PTRACE_CONT, child_id, NULL, NULL ) ;

	//Monitor CAPE program
	while(1) {
		if (cape_wait_for_child_event(child_id, &status) == -1) {
			perror("waitpid");
			break;
		}
		if(WIFEXITED(status)) {
			CAPE_DBG("child exited status=%d", WEXITSTATUS(status));
			break;
		}
		if(WIFSIGNALED(status)) {
			CAPE_DBG("child signaled sig=%d", WTERMSIG(status));
			break;
		}
		if(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
				int dx = 0;
				ptrace(PTRACE_GETREGS, child_id, NULL, &regs);
				dx = (int)(regs.rdx & 0xFFFFFFFFu);
				CAPE_DBG("SIGTRAP dx=%d rip=0x%llx", dx,
					 (unsigned long long)regs.rip);
				CAPE_PROFILE_INC(sigtrap_count);
				switch(dx){
					case S_LOCK_PROCESS_MEMORY:  //Lock process 		
						rc = lock_process_memory(child_id);
						if(process_state == 2) process_state = 3;
						if(rc != 0){
							printf ("Monitor %ld: Error on locking the process image\n",node);
							exit(1);
						}
						break;	
											
					case S_UNLOCK_PROCESS_MEMORY:
						rc = unlock_process_memory(child_id);
						if (rc!=0){
							dprintf("Monitor %ld: Error on unlock process memory\n",node);
							exit(1);
						}
						break;		
									
					case S_GENERATE_CHECKPOINT:
						rc = require_generate_checkpoint();
						if(rc != 0)	exit(1);
						if (cape_ckpt_size_log_enabled()) {
							/* One record per app generate = one per iteration. */
							printf("CKPT_SIZE rank=%ld iter=%ld bytes=%zu\n",
							       node, ++cape_ckpt_size_seq,
							       final_ckpt_size);
							fflush(stdout);
						}
						break;
						
					case S_SEND_CHECKPOINT:	
						rc = require_send_checkpoint();
						if(rc != 0)	exit(1);						
						break;
						
						case S_RECEIVE_CHECKPOINT:
							rc = require_receive_checkpoint();
							if (rc!=0) exit(1);
							break;

						case S_TASK_DEPEND: {
							/* Record one depend() item for the task about to
							 * dispatch. Only the master builds the DAG; workers
							 * issue the same signal but ignore it. */
							unsigned long addr_v = regs.rax;
							int dep_type = (int)((regs.rdx >> 32) & 0xFFu);
							if (node == 0) {
								if (task_pending_dep_count < CAPE_TASK_MAX_DEPS) {
									task_pending_deps[task_pending_dep_count].addr = addr_v;
									task_pending_deps[task_pending_dep_count].type = dep_type;
									task_pending_dep_count++;
									CAPE_DBG("S_TASK_DEPEND addr=0x%lx type=%d pending=%d",
										 addr_v, dep_type, task_pending_dep_count);
								} else {
									fprintf(stderr,
										"Monitor %ld: too many depend() items on one task\n",
										node);
									exit(1);
								}
							}
							break;
						}

						case S_DISPATCH_TASK_CHECKPOINT:
							rc = require_dispatch_task_checkpoint();
							if (rc!=0) exit(1);
							break;

						case S_RECEIVE_TASK_CHECKPOINT:
							rc = require_receive_task_checkpoint();
							if (rc!=0) exit(1);
							break;

						case S_TASK_SPAWN:
							rc = require_task_spawn(regs.rax);
							if (rc!=0) exit(1);
							break;

						case S_TASK_SERVE:
							rc = require_task_serve(regs.rax);
							if (rc!=0) exit(1);
							break;

						case S_TASK_WAIT:
							rc = require_task_wait(regs.rax);
							if (rc!=0) exit(1);
							break;

						case S_TASK_COMPLETE:
							rc = require_task_complete();
							if (rc!=0) exit(1);
							break;

						case S_TASK_REGION_END:
							rc = require_task_region_end();
							if (rc!=0) exit(1);
							break;

						case S_CRITICAL_ENTER:
							rc = require_critical_enter();
							if (rc!=0) exit(1);
							break;

						case S_CRITICAL_EXIT:
							rc = require_critical_exit();
							if (rc!=0) exit(1);
							break;

						case S_INJECT_CHECKPOINT:
							rc = require_inject_checkpoint();
							if (rc!=0)	exit(1);
							break;
					case S_WAIT_FOR_CHECKPOINT:
						rc= require_waitfor_checkpoint();
						if( rc !=0) exit(1);						
						break;
					case S_BROADCAST_CHECKPOINT:
						rc= require_broadcast_checkpoint();
						if( rc !=0) exit(1);						
						break;
					case S_ALL_REDUCE:
						rc = require_allreduce_checkpoint();
						break;
					
					case S_START_SHARE_DATA:												
						read_shared_data();						
						break;
						
					case S_END_SHARE_DATA:
						end_shared_data();
						break;

					case S_REGISTER_REGION: {
						/* Region declared at runtime (after tracking started),
						 * e.g. a task's shared() captures. App already
						 * UFFDIO_REGISTER'd it on the shared uffd; append to
						 * tracked_ranges and write-protect now if live. */
						unsigned long addr_v = regs.rax;
						unsigned long len_v  = regs.rsi;
						struct cape_dickpt_range *grown;
						size_t i;
						int dup = 0;

						for (i = 0; i < tracked_range_count; i++) {
							if (tracked_ranges[i].start == addr_v &&
							    tracked_ranges[i].len == len_v) {
								dup = 1;
								break;
							}
						}
						if (dup)
							break;
						if (tracked_range_count >= CAPE_DICKPT_MAX_RANGES) {
							fprintf(stderr, "Monitor %ld: too many tracked ranges\n",
								node);
							exit(1);
						}
						grown = realloc(tracked_ranges,
								(tracked_range_count + 1) *
								sizeof(*tracked_ranges));
						if (grown == NULL) { perror("realloc(tracked_ranges)"); exit(1); }
						tracked_ranges = grown;
						tracked_ranges[tracked_range_count].start = addr_v;
						tracked_ranges[tracked_range_count].len = len_v;
						tracked_range_count++;
						if (tracking_is_enabled &&
						    cape_userfault_writeprotect(addr_v, len_v, 1) == -1) {
							perror("ioctl(UFFDIO_WRITEPROTECT add)");
							exit(1);
						}
						break;
					}

					case S_DECLARE_REDUCTION: {
						/* rax = addr; rdx high bytes = datatype, op */
						unsigned long addr_v = regs.rax;
						unsigned char dt = (unsigned char)((regs.rdx >> 32) & 0xFFu);
						unsigned char op = (unsigned char)((regs.rdx >> 40) & 0xFFu);
						struct shared_data *sd = malloc(sizeof(*sd));
						if (sd == NULL) { perror("malloc(shared_data)"); exit(1); }
						memset(sd, 0, sizeof(*sd));
						sd->addr = addr_v;
						sd->datatype = dt;
						sd->properties = op;
						sd->len = (unsigned int)reduction_size(dt);
						sd->level = 0;
						sd->prev = data_list_tail;
						sd->next = NULL;
						if (data_list_tail != NULL)
							data_list_tail->next = sd;
						else
							data_list_head = sd;
						data_list_tail = sd;
						break;
					}

					case S_DECLARE_REDUCTION_REGION: {
						/* rax = addr; rsi = byte length; rdx high bytes = datatype, op */
						unsigned long addr_v = regs.rax;
						unsigned long len_v = regs.rsi;
						unsigned char dt = (unsigned char)((regs.rdx >> 32) & 0xFFu);
						unsigned char op = (unsigned char)((regs.rdx >> 40) & 0xFFu);
						struct shared_data *sd = malloc(sizeof(*sd));
						if (len_v > UINT_MAX) {
							fprintf(stderr,
								"Monitor %ld: reduction range too large: %lu bytes\n",
								node, len_v);
							exit(1);
						}
						if (sd == NULL) { perror("malloc(shared_data)"); exit(1); }
						memset(sd, 0, sizeof(*sd));
						sd->addr = addr_v;
						sd->datatype = dt;
						sd->properties = op;
						sd->len = (unsigned int)len_v;
						sd->level = 0;
						sd->prev = data_list_tail;
						sd->next = NULL;
						if (data_list_tail != NULL)
							data_list_tail->next = sd;
						else
							data_list_head = sd;
						data_list_tail = sd;
						break;
					}


					case S_APP_SEND_NUMBER_OF_JOBS: //get number of job from child process
						rc = get_long_int_from_child(&number_of_jobs);
						if(rc!=0){
							printf("Monitor %ld: Error on get number of jobs from child\n", node);
							exit(1);
						}
						rc = init_jobs_per_node();
						if(rc!=0){
							printf("Monitor %ld: Error on initialize jobs for all nodes \n", node);
							exit(1);
						}
						break;
					case S_APP_SEND_TIMESPAN: //read timespan form child				
						rc = get_long_int_from_child(&timespan);
						if(rc != 0){
							printf("Monitor %ld:Error on read timespan form child\n", node);
							exit(1);
						}
						break;
						
					case 95: //read __data_start value of child process					
						rc = get_long_int_from_child(&child_data_start);
						if(rc != 0){
							printf("Monitor %ld:Error on read child_data_start form child\n", node);
							exit(1);
						}						
						
						break;
					case 96: //send checkpoint flag
						rc = send_int_value_to_child(ckpt_flag);
						if(rc!=0){
							printf("Monitor %ld: Error on sending checkpoint flag\n", node);
							exit(0);
						}
						break;					
					case 97:						
						rc = send_int_value_to_child(num_nodes);
						if(rc != 0){
							printf ("Monitor %ld: Error on send num nodes to child\n", node);
							exit(1);
						}
						break;
					case 98:						
						rc = send_int_value_to_child(node);
						if(rc != 0){
							printf ("Monitor %ld: Error on send node value to child\n", node);
							exit(1);
						}
						break;										
					default:
						dprintf("\nMonitor %ld: get breakpoint with unkown edx = %d", node, dx);
						exit(1);
				}
			}//SIGTRAP
		 else if(WIFSTOPPED(status)) {
			int sig = WSTOPSIG(status);
			if (sig == SIGSEGV) {
				fprintf(stderr,
					"Monitor %ld: child received SIGSEGV\n", node);
				kill(child_id, SIGKILL);
				return 1;
			}
			ptrace(PTRACE_CONT, child_id, NULL, (void *)(long)sig);
			continue;
		}
		ptrace(PTRACE_CONT, child_id, NULL, NULL);
	}
	if (userfault_fd >= 0)
		close(userfault_fd);
	if (control_fd >= 0)
		close(control_fd);
	free(tracked_ranges);
	cape_ucx_finalize();
	cape_profile_report();
	return 0;
}

/* ----------------------------------------------------------------
 * clear_list(): clear list of data saved when SIGSEGV occur
 * ----------------------------------------------------------------
 */
int clear_list(struct page_node *list){
	
	if(list == NULL) return 0;
	
	struct page_node * temp_node, * current_node;	
	current_node = list;
	while(current_node){
		temp_node = current_node;
		current_node = current_node->next;
		free(temp_node);
	}
	list_head = NULL;
	list_end = NULL;
	return 0;
}

/* ---------------------------------------------------
 *  send_int_value_to_child(): version 2.0
 * 	Send a value to child process via edx registes
 * ---------------------------------------------------
 */
int send_int_value_to_child(int value){
	struct user_regs_struct child_regs;	
	ptrace(PTRACE_GETREGS, child_id, NULL, &child_regs);	
	child_regs.rdx = value;
	ptrace(PTRACE_SETREGS, child_id, NULL, &child_regs);		
	return 0;
}
/* ----------------------------------------------------
 * get_long_int_from_child(): 
 * 	get a long int number form child process
 * ----------------------------------------------------
 */
int get_long_int_from_child(unsigned long *value){
	struct user_regs_struct child_regs;	
	ptrace(PTRACE_GETREGS, child_id, NULL, &child_regs);	
	*value = child_regs.rax;	
	return 0;
}
/*-------------------------------------------------------------
 * init_jobs_per_node(): initialize number of job per a process
 * ------------------------------------------------------------
 */
int init_jobs_per_node(){	
	int rc = 0;
	if (num_nodes <= 0) {
		rc = 1;
	} else {
		unsigned long jpn = (number_of_jobs + num_nodes - 1) / num_nodes;
		jobs_per_node = (jpn > (unsigned long)INT_MAX) ? INT_MAX : (int)jpn;
	}
	current_job = 0;
	if (rc == 0)
		rc = cape_task_pool_reset();
	return rc;	
	
 }

int ioctl_read_data(unsigned int pid, unsigned long src, void *dst, int len){
	return read_remote_memory(pid, src, dst, (size_t)len) == 0 ? 1 : 0;
}


 int ioctl_set_write_protect(unsigned int pid, unsigned long dst){
	return cape_userfault_writeprotect(dst & ~(PAGE_SIZE - 1), PAGE_SIZE, 1);
}

/*------------------------------------------------------------------
 * void tracer_wait(): v2.0
 * Monitor wait for CAPE program to start-up.
 * -----------------------------------------------------------------
 */
void tracer_wait ( pid_t pid, int * status, int options, struct user * u ){
	static int first = 1 ;
	if ( ! first )
		ptrace ( PTRACE_SYSCALL, pid, NULL, NULL ) ;
	else
		first = 0 ;
	waitpid ( pid, status, options ) ;
	if ( ! WIFEXITED ( * status ) && ! WIFSIGNALED ( * status ) && u )
		ptrace ( PTRACE_GETREGS, pid, NULL, u ) ;
}

/* ------------------------------------------------------------------
 * lock_process_memory(): v2.0
 * 	Monitor set write protected to all pages on PTE the process indicated by pid
 * ------------------------------------------------------------------ 
 */
int lock_process_memory(unsigned int pid){
	if (cape_receive_userfaultfd_setup() != 0)
		return 1;
	tracking_is_enabled = 1;
	return cape_writeprotect_tracked_ranges(1);
}
/* -----------------------------------------------------------------------
 * unlock_process_memory(): version 2.0
 * 	Set writable to all pages of pid process
 * -----------------------------------------------------------------------
 */
int unlock_process_memory(unsigned int pid){
	int rc;

	rc = cape_writeprotect_tracked_ranges(0);
	tracking_is_enabled = 0;
	return rc;
}

/* -------------------------------------------------------------
 * read_shared_data(): read shared variables from deriver memory,
 * 	and save to a list manager by data_list_head and data_list_tail 
 *
 * -------------------------------------------------------------
 */
int read_shared_data(){
	/* App-side protocol for S_START_SHARE_DATA:
	 *   rax = start address of the shared region
	 *   rsi = length in bytes
	 *   rcx = level (0 = global, >0 = scope-local; matched by S_END_SHARE_DATA)
	 * Append a node to data_list_head/data_list_tail. Used by
	 * is_address_shared() to mask private words during checkpoint generation. */
	struct user_regs_struct regs;
	struct shared_data *sd;

	ptrace(PTRACE_GETREGS, child_id, NULL, &regs);

	sd = malloc(sizeof(*sd));
	if (sd == NULL) { perror("malloc(shared_data)"); exit(1); }
	memset(sd, 0, sizeof(*sd));
	sd->addr = (unsigned long)regs.rax;
	sd->len  = (unsigned int)regs.rsi;
	sd->level = (unsigned char)(regs.rcx & 0xFFu);
	sd->prev = data_list_tail;
	sd->next = NULL;
	if (data_list_tail != NULL)
		data_list_tail->next = sd;
	else
		data_list_head = sd;
	data_list_tail = sd;
	return 0;
}
/* ---------------------------------------------------------------------
 * end_shared_data: to close the sharing data off current level
 * 
 * ---------------------------------------------------------------------
 */
void end_shared_data(){
	/* App-side protocol for S_END_SHARE_DATA:
	 *   rax = level to drop (every shared_data node with this level is freed).
	 * Walks the list and unlinks/free's matching nodes. Safe to call with no
	 * matching entries (no-op). */
	struct user_regs_struct regs;
	struct shared_data *p, *next;
	unsigned char level;

	if (data_list_head == NULL)
		return;

	ptrace(PTRACE_GETREGS, child_id, NULL, &regs);
	level = (unsigned char)(regs.rax & 0xFFu);

	p = data_list_head;
	while (p != NULL) {
		next = p->next;
		if (p->level == level) {
			if (p->prev) p->prev->next = p->next; else data_list_head = p->next;
			if (p->next) p->next->prev = p->prev; else data_list_tail = p->prev;
			free(p);
		}
		p = next;
	}
}
 
/*-------------------------------------------------------------------------
 * generate_checkpoint() : version 5.0
 * 	generate DICKPT and save to a stream memory file
 * 	This function use to replace the save_checkpoint function of CAPE version 2
 * 	Structure of checkpoint file: <registers info>,<addr, len, data>, <addr, len, data>....
 * 	This function also set write protect to all pages that have been unlocked by clear_write_protect function
 * parametters:
 * 	+ child_id: id process
 * 	+ * list  : a pointer that point to list of data saved before execute command
 * return: the result will be contained in two global variables
 * 	+ A point to stream memory file
 * 	+ Size of this file
 * 
 * Checkpoint files will be divided into two part: C = S + L
 * + S: memory that is not in sharing-data attributes list
 * + L: contain only data in sharing-data attributes list
 * ------------------------------------------------------------------------
 */
 FILE *generate_checkpoint(int child_id, 
 	struct page_node * list,
 	unsigned char **ckpt_data,
 	size_t *ckpt_size,
 	unsigned char cflag,
 	unsigned long tsp){	

	FILE *stream;
	struct page_node *old_node;
	unsigned long _timespan;
	unsigned long marker = BMP_S;
	CAPE_PROFILE_NS_VAR(profile_start_ns);
	CAPE_PROFILE_NS_START(profile_start_ns);

	struct pack_page {
		uint64_t  addr;
		uint8_t   bmp[BMP_WORD_BMP_BYTES];
		uint32_t  n_changed;
		uint32_t *changed;
	};
	struct pack_page *pp = NULL;
	size_t cap = 0, n = 0, k;

	//open the stream memory file
	if (ckpt_data == &final_ckpt) {
		stream = cape_final_open_scratch_stream(ckpt_data, ckpt_size);
	} else {
		stream = open_binary_memstream(ckpt_data, ckpt_size);
	}
	if (stream == NULL) {
		fprintf(stderr, "Monitor %ld: failed to open checkpoint stream\n", node);
		exit(1);
	}

	//get the registers
	ptrace(PTRACE_GETREGS, child_id, NULL, &save_regs);

	//write timespan into checkpoint
	_timespan = tsp;
	fwrite(&_timespan, sizeof(unsigned long), 1, stream);

	//save register to file
	fwrite(&save_regs,sizeof(struct user_regs_struct),1, stream);

	for (old_node = list; old_node != NULL; old_node = old_node->next)
		cap++;
	if (cap != 0) {
		pp = calloc(cap, sizeof(*pp));
		if (pp == NULL) { perror("calloc(pack_page)"); exit(1); }
	}

	for (old_node = list; old_node != NULL; old_node = old_node->next) {
		unsigned char current_page[BMP_PAGE_SIZE];
		uint32_t tmp_changed[BMP_WORDS_PER_PAGE];
		const uint32_t *pre  = (const uint32_t *)old_node->data;
		const uint32_t *post = (const uint32_t *)current_page;
		unsigned w, nc = 0;

		if (read_remote_memory(child_id, old_node->addr,
				       current_page, BMP_PAGE_SIZE) != 0) {
			fprintf(stderr,
				"Monitor %ld: failed to read dirty page 0x%lx\n",
				node, old_node->addr);
			continue;
		}

		memset(pp[n].bmp, 0, BMP_WORD_BMP_BYTES);
		for (w = 0; w < BMP_WORDS_PER_PAGE; ++w) {
			unsigned long waddr = old_node->addr + (unsigned long)w * 4u;
			if (!is_address_shared(waddr))
				continue;
			if (pre[w] != post[w]) {
				wbmp_set(pp[n].bmp, w);
				tmp_changed[nc++] = post[w];
			}
		}
		if (nc == 0)
			continue;

		pp[n].addr = (uint64_t)old_node->addr;
		pp[n].n_changed = nc;
		pp[n].changed = malloc((size_t)nc * sizeof(uint32_t));
		if (pp[n].changed == NULL) { perror("malloc(changed)"); exit(1); }
		memcpy(pp[n].changed, tmp_changed, (size_t)nc * sizeof(uint32_t));
		n++;
	}

	{
		uint32_t n32 = (uint32_t)n;
		fwrite(&marker, sizeof(marker), 1, stream);
		fwrite(&n32, sizeof(n32), 1, stream);
		for (k = 0; k < n; ++k)
			fwrite(&pp[k].addr, sizeof(pp[k].addr), 1, stream);
		for (k = 0; k < n; ++k) {
			fwrite(pp[k].bmp, BMP_WORD_BMP_BYTES, 1, stream);
			if (pp[k].n_changed != 0)
				fwrite(pp[k].changed,
				       (size_t)pp[k].n_changed * sizeof(uint32_t),
				       1, stream);
			free(pp[k].changed);
		}
	}
	free(pp);

	for (old_node = list; old_node != NULL; old_node = old_node->next) {
		ioctl_set_write_protect(child_id, old_node->addr);
	}

	fflush(stream);
	if (ferror(stream)) {
		fprintf(stderr,
			"Monitor %ld: checkpoint stream write failed; "
			"increase CAPE_UCX_SCRATCH_BYTES\n",
			node);
		exit(1);
	}
	{
		long pos = ftell(stream);
		if (pos < 0) {
			perror("ftell(checkpoint stream)");
			exit(1);
		}
		*ckpt_size = (size_t)pos;
	}
	cape_profile_record_generated_ckpt(*ckpt_size);
	CAPE_PROFILE_ADD_NS(generate_ckpt_ns, profile_start_ns);
	CAPE_PROFILE_INC(generate_ckpt_calls);
	return stream;
}

/* ----------------------------------------------------------------------
 * require_generate_checkpoint(): Version 3.0 => 4.0 => 5.0
 * 	1. generate checkpoint and save into a memory file stream (final_ckpt)
 * 	2. re-lock pages
 * 	3. clear list
 * -----------------------------------------------------------------------
 */
int require_generate_checkpoint(){
	int rc=0;

	CAPE_DBG("require_generate_checkpoint enter");
	rc = cape_drain_userfaultfd();
	if (rc != 0)
		return rc;

	//generate new check point	
    final_ckpt_stream = generate_checkpoint(child_id,
											list_head,
											&final_ckpt,
											&final_ckpt_size,
											EXIT_CHECKPOINT,
											timespan);
	

	
	clear_list(list_head);
	list_head = NULL;	
	

	
	if(rc!=0) printf("Monitor %ld: Error on requiring generate checkpoint\n", node);	
	CAPE_DBG("require_generate_checkpoint exit rc=%d final_size=%zu",
		 rc, final_ckpt_size);
	return rc;
}



/* ---------------------------------------
 * send_checkpoint(): send checkpoint file to destination node
 * 	This function use to replace a part of function save_checkpoint of vesion 2.0
 * Parametters:
 * 	- destination: rank of destination node
 * Returns: N/A
 * TODO: divide this file to small size if the checkpoint file is too large.*
 * --------------------------------------
 */
static void release_final_checkpoint(void)
{
	if (final_ckpt_stream != NULL) {
		fclose(final_ckpt_stream);
		final_ckpt_stream = NULL;
	}
	if (!final_ckpt_uses_scratch)
		free(final_ckpt);
	final_ckpt = NULL;
	final_ckpt_size = 0;
	final_ckpt_uses_scratch = 0;
}

int send_checkpoint(int destination){
	CAPE_DBG("send_checkpoint destination=%d size=%zu",
		 destination, final_ckpt_size);
	cape_ucx_send(final_ckpt, final_ckpt_size, destination, TAG_CKPT_DATA);
	release_final_checkpoint();
	return 0;
}

/* Tear down the dependency DAG between OpenMP scopes. */
static void cape_task_dag_reset(void)
{
	struct cape_dep_obj *p = task_dep_objs, *next;

	while (p != NULL) {
		next = p->next;
		free(p->readers);
		free(p);
		p = next;
	}
	task_dep_objs = NULL;
	task_pending_dep_count = 0;
	task_next_id = 0;
	free(task_of_worker);
	task_of_worker = NULL;
	free(task_done);
	task_done = NULL;
	task_done_cap = 0;
}

static struct cape_dep_obj *cape_dep_obj_get(unsigned long addr)
{
	struct cape_dep_obj *p;

	for (p = task_dep_objs; p != NULL; p = p->next)
		if (p->addr == addr)
			return p;
	p = malloc(sizeof(*p));
	if (p == NULL) { perror("malloc(dep_obj)"); exit(1); }
	p->addr = addr;
	p->last_writer = -1;
	p->readers = NULL;
	p->n_readers = 0;
	p->cap_readers = 0;
	p->next = task_dep_objs;
	task_dep_objs = p;
	return p;
}

static void cape_dep_obj_add_reader(struct cape_dep_obj *o, long task_id)
{
	if (o->n_readers == o->cap_readers) {
		int ncap = o->cap_readers ? o->cap_readers * 2 : 4;
		long *grown = realloc(o->readers, (size_t)ncap * sizeof(long));
		if (grown == NULL) { perror("realloc(readers)"); exit(1); }
		o->readers = grown;
		o->cap_readers = ncap;
	}
	o->readers[o->n_readers++] = task_id;
}

/* Record that task_id is now running on worker, and ensure task_done can be
 * indexed by task_id. */
static void cape_task_mark_dispatched(long task_id, int worker)
{
	if ((size_t)task_id >= task_done_cap) {
		size_t ncap = task_done_cap ? task_done_cap * 2 : 64;
		unsigned char *grown;
		while ((size_t)task_id >= ncap)
			ncap *= 2;
		grown = realloc(task_done, ncap);
		if (grown == NULL) { perror("realloc(task_done)"); exit(1); }
		memset(grown + task_done_cap, 0, ncap - task_done_cap);
		task_done = grown;
		task_done_cap = ncap;
	}
	task_done[task_id] = 0;
	if (task_of_worker != NULL)
		task_of_worker[worker] = task_id;
}

static int cape_task_pool_reset(void)
{
	int i;
	CAPE_DBG("task_pool_reset enter num_nodes=%d", num_nodes);

	free(task_idle_workers);
	free(task_worker_busy);
	task_idle_workers = NULL;
	task_worker_busy = NULL;
	task_idle_count = 0;
	task_active_workers = 0;
	task_pool_ready = 0;
	task_dispatched_jobs = 0;
	task_completed_jobs = 0;
	cape_task_dag_reset();
	cape_dyn_reset();

	if (node != 0 || num_nodes <= 1) {
		task_pool_ready = 1;
		return 0;
	}

	task_idle_workers = calloc((size_t)num_nodes, sizeof(*task_idle_workers));
	task_worker_busy = calloc((size_t)num_nodes, sizeof(*task_worker_busy));
	task_of_worker = malloc((size_t)num_nodes * sizeof(*task_of_worker));
	if (task_idle_workers == NULL || task_worker_busy == NULL ||
	    task_of_worker == NULL) {
		free(task_idle_workers);
		free(task_worker_busy);
		free(task_of_worker);
		task_idle_workers = NULL;
		task_worker_busy = NULL;
		task_of_worker = NULL;
		return 1;
	}
	for (i = 0; i < num_nodes; ++i)
		task_of_worker[i] = -1;

	for (i = 1; i < num_nodes; ++i)
		task_idle_workers[task_idle_count++] = i;
	task_pool_ready = 1;
	CAPE_DBG("task_pool_reset done idle_count=%d", task_idle_count);
	return 0;
}

static int cape_task_record_completion(int worker, unsigned char *payload,
				       size_t payload_size)
{
	FILE *stream;
	int rc;
	CAPE_DBG("task completion from worker=%d payload_size=%zu",
		 worker, payload_size);

	if (worker <= 0 || worker >= num_nodes) {
		fprintf(stderr,
			"Monitor %ld: wildcard completion from invalid worker %d\n",
			node, worker);
		free(payload);
		return 1;
	}
	if (task_worker_busy == NULL || task_worker_busy[worker] == 0) {
		fprintf(stderr,
			"Monitor %ld: unexpected completion from idle worker %d\n",
			node, worker);
		free(payload);
		return 1;
	}
	if (payload_size == 0) {
		fprintf(stderr,
			"Monitor %ld: empty task checkpoint from worker %d\n",
			node, worker);
		free(payload);
		return 1;
	}

	stream = fmemopen(payload, payload_size, "rb");
	if (stream == NULL) {
		free(payload);
		return 1;
	}
	rc = merge_external_checkpoint(stream, payload, payload_size);
	fclose(stream);
	free(payload);
	if (rc != 0)
		return rc;

	task_worker_busy[worker] = 0;
	task_idle_workers[task_idle_count++] = worker;
	task_active_workers--;
	task_completed_jobs++;

	/* Mark this worker's task complete so dependent tasks waiting on it can
	 * be dispatched (its output is now merged into total_ckpt). */
	if (task_of_worker != NULL && task_of_worker[worker] >= 0) {
		long tid = task_of_worker[worker];
		if ((size_t)tid < task_done_cap)
			task_done[tid] = 1;
		task_of_worker[worker] = -1;
	}

	CAPE_DBG("task completion merged worker=%d completed=%lu active=%d idle=%d",
		 worker, task_completed_jobs, task_active_workers, task_idle_count);
	return 0;
}

static int cape_task_wait_for_completion(void)
{
	unsigned char *payload;
	size_t payload_size = 0;
	int worker = -1;

	payload = cape_ucx_recv_probe_alloc_any(&payload_size, &worker,
						TAG_CKPT_DATA);
	CAPE_DBG("task wait completion received worker=%d size=%zu",
		 worker, payload_size);
	return cape_task_record_completion(worker, payload, payload_size);
}

static int cape_task_get_idle_worker(int *worker)
{
	int rc;

	if (!task_pool_ready) {
		CAPE_DBG("task pool not ready, resetting");
		rc = cape_task_pool_reset();
		if (rc != 0)
			return rc;
	}

	while (task_idle_count == 0) {
		CAPE_DBG("no idle worker active=%d completed=%lu dispatched=%lu",
			 task_active_workers, task_completed_jobs,
			 task_dispatched_jobs);
		if (task_active_workers <= 0)
			return 1;
		rc = cape_task_wait_for_completion();
		if (rc != 0)
			return rc;
	}

	*worker = task_idle_workers[--task_idle_count];
	CAPE_DBG("selected idle worker=%d idle_left=%d", *worker, task_idle_count);
	return 0;
}

static int cape_task_dispatch_checkpoint(int worker)
{
	int rc;

	if (worker <= 0 || worker >= num_nodes)
		return 1;
	if (task_worker_busy[worker])
		return 1;

	current_job++;
	task_dispatched_jobs++;
	CAPE_DBG("dispatch checkpoint to worker=%d job=%d/%lu size=%zu",
		 worker, current_job, number_of_jobs, final_ckpt_size);

	/* The checkpoint is the task payload. The legacy flag still tells the
	 * worker whether the global task stream has more checkpoints after this
	 * dispatch; completion ownership itself is now discovered by wildcard
	 * receives on TAG_CKPT_DATA. */
	ckpt_flag = ((unsigned long)current_job < number_of_jobs) ? 1 : 0;
	current_node = worker;

	rc = send_checkpoint(worker);
	if (rc != 0)
		return rc;
	cape_ucx_send(&ckpt_flag, sizeof(int), worker, TAG_CKPT_FLAG);

	task_worker_busy[worker] = 1;
	task_active_workers++;
	dprintf("Monitor %ld: dispatched task %d/%lu to worker %d (flag=%d)\n",
		node, current_job, number_of_jobs, worker, ckpt_flag);
	return 0;
}

static int cape_task_dispatch_directive(int worker)
{
	unsigned char skip_msg = CAPE_TASK_SKIP;
	unsigned char *run_msg = NULL;
	const unsigned char *src;
	size_t src_size;
	size_t run_size;
	int i;

	if (worker <= 0 || worker >= num_nodes)
		return 1;
	if (task_worker_busy[worker])
		return 1;
	if (final_ckpt_stream != NULL)
		fflush(final_ckpt_stream);
	if (final_ckpt == NULL || final_ckpt_size == 0)
		return 1;

	/* Ship the accumulated checkpoint of all completed tasks (total_ckpt)
	 * when present, so a dependent task's worker observes its predecessors'
	 * outputs after the dispatch barrier waited for them. Independent tasks
	 * also receive it harmlessly: injected (clean) pages are not re-dirtied,
	 * so the worker only sends back its own writes. The first task (empty
	 * accumulator) falls back to the master's base checkpoint header. */
	if (total_ckpt_stream != NULL) {
		fflush(total_ckpt_stream);
		cape_total_stream_update_size();
	}
	if (total_ckpt != NULL && total_ckpt_size > 0) {
		src = total_ckpt;
		src_size = total_ckpt_size;
	} else {
		src = final_ckpt;
		src_size = final_ckpt_size;
	}
	if (src_size == 0 || src_size > SIZE_MAX - 1)
		return 1;

	run_size = src_size + 1;
	run_msg = malloc(run_size);
	if (run_msg == NULL)
		return 1;
	run_msg[0] = CAPE_TASK_RUN;
	memcpy(run_msg + 1, src, src_size);
	CAPE_DBG("dispatch_directive worker=%d ships=%s size=%zu total_size=%zu",
		 worker, (src == final_ckpt) ? "final" : "total",
		 src_size, total_ckpt_size);

	current_job++;
	task_dispatched_jobs++;
	current_node = worker;
	for (i = 1; i < num_nodes; ++i) {
		if (i == worker)
			cape_ucx_send(run_msg, run_size, i, TAG_CKPT_DATA);
		else
			cape_ucx_send(&skip_msg, sizeof(skip_msg), i,
				      TAG_CKPT_DATA);
	}
	CAPE_DBG("task directive messages sent worker=%d run_size=%zu num_nodes=%d",
		 worker, run_size, num_nodes);
	free(run_msg);
	release_final_checkpoint();

	task_worker_busy[worker] = 1;
	task_active_workers++;
	dprintf("Monitor %ld: scheduled task directive %d to worker %d\n",
		node, current_job, worker);
	return 0;
}

/* Wait until task pid has completed (merged into total_ckpt). Drains task
 * completions in the meantime. pid is always an earlier (already-dispatched)
 * task, so it is either done or in flight on some worker — never deadlocks. */
static int cape_task_wait_for_task(long pid)
{
	if (pid < 0 || (size_t)pid >= task_done_cap)
		return 0;
	while (!task_done[pid]) {
		int rc;
		if (task_active_workers <= 0)
			return 0;   /* nothing in flight; treat as done */
		rc = cape_task_wait_for_completion();
		if (rc != 0)
			return rc;
	}
	return 0;
}

/* Resolve the pending depend() items into predecessor task ids, block until
 * they finish, then fold this task into the DAG. Returns the new task id. */
static int cape_task_resolve_deps(long this_id)
{
	long *preds;
	int n_preds = 0;
	int i, j, rc = 0;

	/* At most this_id distinct earlier tasks can be predecessors. */
	preds = malloc((size_t)(this_id > 0 ? this_id : 1) * sizeof(long));
	if (preds == NULL) { perror("malloc(preds)"); exit(1); }

#define CAPE_ADD_PRED(p) do { \
		long _p = (p); \
		int _seen = 0, _k; \
		if (_p >= 0 && _p != this_id) { \
			for (_k = 0; _k < n_preds; ++_k) \
				if (preds[_k] == _p) { _seen = 1; break; } \
			if (!_seen) \
				preds[n_preds++] = _p; \
		} \
	} while (0)

	/* Derive predecessors (read the DAG before mutating it). */
	for (i = 0; i < task_pending_dep_count; ++i) {
		struct cape_dep_obj *o = cape_dep_obj_get(task_pending_deps[i].addr);
		if (task_pending_deps[i].type == CAPE_DEP_IN) {
			CAPE_ADD_PRED(o->last_writer);            /* RAW */
		} else {
			CAPE_ADD_PRED(o->last_writer);            /* WAW */
			for (j = 0; j < o->n_readers; ++j)
				CAPE_ADD_PRED(o->readers[j]);     /* WAR */
		}
	}

	CAPE_DBG("resolve_deps this_id=%ld pending=%d n_preds=%d",
		 this_id, task_pending_dep_count, n_preds);
	for (i = 0; i < n_preds; ++i) {
		CAPE_DBG("  wait this_id=%ld on pred=%ld done=%d active=%d cap=%zu",
			 this_id, preds[i],
			 ((size_t)preds[i] < task_done_cap) ? task_done[preds[i]] : -1,
			 task_active_workers, task_done_cap);
		rc = cape_task_wait_for_task(preds[i]);
		if (rc != 0) {
			free(preds);
			return rc;
		}
	}
	free(preds);

	/* Fold this task into the DAG: writers reset the readers history. */
	for (i = 0; i < task_pending_dep_count; ++i) {
		struct cape_dep_obj *o = cape_dep_obj_get(task_pending_deps[i].addr);
		if (task_pending_deps[i].type == CAPE_DEP_IN) {
			cape_dep_obj_add_reader(o, this_id);
		} else {
			o->last_writer = this_id;
			o->n_readers = 0;
		}
	}
	task_pending_dep_count = 0;
#undef CAPE_ADD_PRED
	return 0;
}

int require_dispatch_task_checkpoint()
{
	int rc = 0;
	int worker = -1;
	long this_id;
	CAPE_DBG("require_dispatch_task_checkpoint enter pending_deps=%d",
		 task_pending_dep_count);

	if (node != 0) {
		task_pending_dep_count = 0;
		return 0;
	}
	if (num_nodes <= 1) {
		fprintf(stderr,
			"Monitor %ld: OpenMP task requires at least one worker rank\n",
			node);
		release_final_checkpoint();
		return 1;
	}

	if (!task_pool_ready) {
		rc = cape_task_pool_reset();
		if (rc != 0) {
			release_final_checkpoint();
			return rc;
		}
	}

	this_id = task_next_id++;

	/* Honor depend(): block this dispatch until predecessor tasks complete
	 * and merge, so the accumulated total_ckpt we ship carries the inputs. */
	rc = cape_task_resolve_deps(this_id);
	if (rc != 0) {
		printf("Monitor %ld: Error resolving task dependencies\n", node);
		release_final_checkpoint();
		return rc;
	}

	rc = cape_task_get_idle_worker(&worker);
	if (rc == 0)
		rc = cape_task_dispatch_directive(worker);
	if (rc == 0)
		cape_task_mark_dispatched(this_id, worker);
	if (rc != 0)
		printf("Monitor %ld: Error on task checkpoint dispatch\n", node);
	CAPE_DBG("require_dispatch_task_checkpoint exit rc=%d task_id=%ld", rc, this_id);
	return rc;
}


int require_send_checkpoint(){
	int rc = 0;

	if (node == 0) {
		int worker = -1;

		if (num_nodes <= 1) {
			release_final_checkpoint();
			return 0;
		}
		if (number_of_jobs != 0 &&
		    (unsigned long)current_job >= number_of_jobs) {
			fprintf(stderr,
				"Monitor %ld: no queued task left for checkpoint dispatch "
				"(current=%d total=%lu)\n",
				node, current_job, number_of_jobs);
			release_final_checkpoint();
			return 1;
		}

		rc = cape_task_get_idle_worker(&worker);
		if (rc == 0)
			rc = cape_task_dispatch_checkpoint(worker);
	} else {
		/* Worker completion: every task ends with a checkpoint back to
		 * master. Rank 0 matches this with a token-only wildcard receive,
		 * which is the control plane that identifies the finished worker. */
		rc = send_checkpoint(0);
	}

	if (rc != 0)
		printf("Monitor %ld: Error on sending checkpoint \n", node);
		return rc;
	}

int require_receive_task_checkpoint()
{
	unsigned char *packet;
	size_t packet_size = 0;
	unsigned long assigned = 0;
	int rc = 0;
	CAPE_DBG("require_receive_task_checkpoint enter");

	if (node == 0)
		return send_int_value_to_child(0);

	packet = cape_ucx_recv_probe_alloc(&packet_size, 0, TAG_CKPT_DATA);
	CAPE_DBG("require_receive_task_checkpoint got packet size=%zu opcode=%u",
		 packet_size, packet_size >= 1 ? (unsigned)packet[0] : 999u);
	if (packet_size < 1) {
		free(packet);
		return 1;
	}

	if (packet[0] == CAPE_TASK_RUN) {
		size_t payload_size = packet_size - 1;

		if (payload_size == 0) {
			free(packet);
			return 1;
		}
		if (after_ckpt_stream != NULL) {
			fclose(after_ckpt_stream);
			after_ckpt_stream = NULL;
		}
		free(after_ckpt);
		after_ckpt = NULL;
		after_ckpt_size = 0;
		after_ckpt_stream = open_binary_memstream(&after_ckpt,
							  &after_ckpt_size);
		if (after_ckpt_stream == NULL) {
			free(packet);
			return 1;
		}
		if (fwrite(packet + 1, payload_size, 1, after_ckpt_stream) != 1) {
			fclose(after_ckpt_stream);
			after_ckpt_stream = NULL;
			free(after_ckpt);
			after_ckpt = NULL;
			after_ckpt_size = 0;
			free(packet);
			return 1;
		}
		fflush(after_ckpt_stream);
		assigned = 1;
	} else if (packet[0] != CAPE_TASK_SKIP) {
		fprintf(stderr,
			"Monitor %ld: unknown task control opcode %u\n",
			node, (unsigned)packet[0]);
		rc = 1;
	}

	free(packet);
	if (rc == 0)
		rc = send_int_value_to_child((int)assigned);
	CAPE_DBG("require_receive_task_checkpoint exit assigned=%lu rc=%d",
		 assigned, rc);
	return rc;
}

/* ==========================================================================
 * Dynamic (nested/recursive) task scheduler.
 *
 * Static tasks need every rank at the same lexical dispatch point; recursive
 * task graphs break that. Here the master is the single scheduler with a
 * task queue, per-worker run stacks and taskwait bookkeeping:
 *   - spawn: master enqueues directly; a worker ships its delta checkpoint
 *     plus the task descriptor to the master (fire-and-forget).
 *   - idle workers serve RUN/SHUTDOWN directives; RUN injects the
 *     accumulated checkpoint, then the app calls desc.fn(desc.args).
 *   - taskwait: the master replies DONE (+ total ckpt) once the waiter's
 *     direct children completed, or hands the waiter a queued task to run
 *     inline — so a tree of waiting parents can never deadlock the pool.
 * Worker -> master messages share TAG_TASK_CTRL so they stay FIFO per
 * worker; consistency depends on COMPLETE being seen after the task's
 * SUBMITs and before the worker's next WAIT.
 * ==========================================================================
 */
static int inject_checkpoint_with_write_access(FILE *stream, size_t *file_size,
		struct user_regs_struct *regs);

#define CAPE_DYN_DESC_HDR (2 * sizeof(unsigned long))   /* fn + args_size */

struct cape_dyn_task {
	long id;
	long parent;                 /* task id, -1 = master (root) */
	unsigned long fn;
	unsigned long args_size;
	unsigned char args[DICKPT_TASK_ARGS_MAX];
	struct cape_dyn_task *next;
};

static struct cape_dyn_task *dyn_q_head, *dyn_q_tail;
static int dyn_q_len;
static long dyn_next_id;
static long dyn_root_children;          /* outstanding direct children of root */
static long *dyn_parent_of;             /* task id -> parent id */
static long *dyn_kids;                  /* task id -> outstanding direct children */
static size_t dyn_task_cap;
static unsigned char *dyn_waiting;      /* worker -> blocked in taskwait, listening */
static long **dyn_stk;                  /* per-worker run stack (inline nesting) */
static int *dyn_stk_n, *dyn_stk_cap;
static int dyn_ready;

static void cape_dyn_reset(void)
{
	struct cape_dyn_task *t = dyn_q_head, *next;
	int w;

	while (t != NULL) {
		next = t->next;
		free(t);
		t = next;
	}
	dyn_q_head = dyn_q_tail = NULL;
	dyn_q_len = 0;
	dyn_next_id = 0;
	dyn_root_children = 0;
	free(dyn_parent_of);
	free(dyn_kids);
	dyn_parent_of = NULL;
	dyn_kids = NULL;
	dyn_task_cap = 0;
	free(dyn_waiting);
	dyn_waiting = NULL;
	if (dyn_stk != NULL) {
		for (w = 0; w < num_nodes; ++w)
			free(dyn_stk[w]);
	}
	free(dyn_stk);
	free(dyn_stk_n);
	free(dyn_stk_cap);
	dyn_stk = NULL;
	dyn_stk_n = NULL;
	dyn_stk_cap = NULL;
	dyn_ready = 0;
}

static int cape_dyn_init(void)
{
	if (dyn_ready)
		return 0;
	if (!task_pool_ready && cape_task_pool_reset() != 0)
		return 1;
	if (num_nodes <= 1) {
		fprintf(stderr,
			"Monitor %ld: dynamic OpenMP tasks need at least one worker rank\n",
			node);
		return 1;
	}
	dyn_waiting = calloc((size_t)num_nodes, 1);
	dyn_stk = calloc((size_t)num_nodes, sizeof(*dyn_stk));
	dyn_stk_n = calloc((size_t)num_nodes, sizeof(*dyn_stk_n));
	dyn_stk_cap = calloc((size_t)num_nodes, sizeof(*dyn_stk_cap));
	if (dyn_waiting == NULL || dyn_stk == NULL || dyn_stk_n == NULL ||
	    dyn_stk_cap == NULL)
		return 1;
	dyn_ready = 1;
	return 0;
}

static void cape_dyn_grow_tasks(long id)
{
	if ((size_t)id < dyn_task_cap)
		return;
	{
		size_t ncap = dyn_task_cap ? dyn_task_cap * 2 : 64;
		long *p, *k;
		size_t i;
		while ((size_t)id >= ncap)
			ncap *= 2;
		p = realloc(dyn_parent_of, ncap * sizeof(long));
		k = realloc(dyn_kids, ncap * sizeof(long));
		if (p == NULL || k == NULL) { perror("realloc(dyn tasks)"); exit(1); }
		for (i = dyn_task_cap; i < ncap; ++i) { p[i] = -1; k[i] = 0; }
		dyn_parent_of = p;
		dyn_kids = k;
		dyn_task_cap = ncap;
	}
}

static long cape_dyn_enqueue(long parent, unsigned long fn,
			     unsigned long args_size, const unsigned char *args)
{
	struct cape_dyn_task *t = malloc(sizeof(*t));

	if (t == NULL) { perror("malloc(dyn_task)"); exit(1); }
	t->id = dyn_next_id++;
	t->parent = parent;
	t->fn = fn;
	t->args_size = args_size;
	memcpy(t->args, args, args_size);
	t->next = NULL;
	if (dyn_q_tail != NULL)
		dyn_q_tail->next = t;
	else
		dyn_q_head = t;
	dyn_q_tail = t;
	dyn_q_len++;

	cape_dyn_grow_tasks(t->id);
	dyn_parent_of[t->id] = parent;
	dyn_kids[t->id] = 0;
	if (parent < 0)
		dyn_root_children++;
	else
		dyn_kids[parent]++;
	CAPE_DBG("dyn enqueue id=%ld parent=%ld fn=0x%lx qlen=%d",
		 t->id, parent, fn, dyn_q_len);
	return t->id;
}

static struct cape_dyn_task *cape_dyn_dequeue(void)
{
	struct cape_dyn_task *t = dyn_q_head;
	if (t == NULL)
		return NULL;
	dyn_q_head = t->next;
	if (dyn_q_head == NULL)
		dyn_q_tail = NULL;
	dyn_q_len--;
	return t;
}

static void cape_dyn_push(int w, long id)
{
	if (dyn_stk_n[w] == dyn_stk_cap[w]) {
		int ncap = dyn_stk_cap[w] ? dyn_stk_cap[w] * 2 : 8;
		long *grown = realloc(dyn_stk[w], (size_t)ncap * sizeof(long));
		if (grown == NULL) { perror("realloc(dyn stack)"); exit(1); }
		dyn_stk[w] = grown;
		dyn_stk_cap[w] = ncap;
	}
	dyn_stk[w][dyn_stk_n[w]++] = id;
}

/* Flush + expose the accumulated checkpoint (may be empty early on). */
static void cape_dyn_total_view(const unsigned char **p, size_t *n)
{
	if (total_ckpt_stream != NULL) {
		fflush(total_ckpt_stream);
		cape_total_stream_update_size();
	}
	if (total_ckpt != NULL && total_ckpt_size > 0) {
		*p = total_ckpt;
		*n = total_ckpt_size;
	} else {
		*p = NULL;
		*n = 0;
	}
}

/* Merge a checkpoint byte range into total_ckpt. */
static int cape_dyn_merge_bytes(unsigned char *data, size_t size)
{
	FILE *s;
	int rc;

	if (size == 0)
		return 0;
	s = fmemopen(data, size, "rb");
	if (s == NULL)
		return 1;
	rc = merge_external_checkpoint(s, data, size);
	fclose(s);
	return rc;
}

/* Send a RUN directive (descriptor + accumulated ckpt) and record the task
 * on the worker's run stack. Caller manages idle/busy transitions. */
static int cape_dyn_send_run(int worker, struct cape_dyn_task *t)
{
	const unsigned char *tot;
	size_t tn, hdr, mlen;
	unsigned char *msg;

	cape_dyn_total_view(&tot, &tn);
	hdr = 1 + CAPE_DYN_DESC_HDR + t->args_size;
	mlen = hdr + tn;
	msg = malloc(mlen);
	if (msg == NULL)
		return 1;
	msg[0] = CAPE_TASK_RUN_DYN;
	memcpy(msg + 1, &t->fn, sizeof(unsigned long));
	memcpy(msg + 1 + sizeof(unsigned long), &t->args_size,
	       sizeof(unsigned long));
	memcpy(msg + 1 + CAPE_DYN_DESC_HDR, t->args, t->args_size);
	if (tn != 0)
		memcpy(msg + hdr, tot, tn);
	CAPE_DBG("dyn run -> worker=%d id=%ld parent=%ld ckpt=%zu",
		 worker, t->id, t->parent, tn);
	cape_ucx_send(msg, mlen, worker, TAG_CKPT_DATA);
	free(msg);
	cape_dyn_push(worker, t->id);
	task_dispatched_jobs++;
	return 0;
}

static int cape_dyn_send_done(int worker)
{
	const unsigned char *tot;
	size_t tn, mlen;
	unsigned char *msg;

	cape_dyn_total_view(&tot, &tn);
	mlen = 1 + tn;
	msg = malloc(mlen);
	if (msg == NULL)
		return 1;
	msg[0] = CAPE_TASK_DONE;
	if (tn != 0)
		memcpy(msg + 1, tot, tn);
	CAPE_DBG("dyn done -> worker=%d ckpt=%zu", worker, tn);
	cape_ucx_send(msg, mlen, worker, TAG_CKPT_DATA);
	free(msg);
	return 0;
}

/* Hand queued tasks to idle workers. */
static int cape_dyn_try_dispatch(void)
{
	while (dyn_q_len > 0 && task_idle_count > 0) {
		int w = task_idle_workers[--task_idle_count];
		struct cape_dyn_task *t = cape_dyn_dequeue();
		int rc;

		task_worker_busy[w] = 1;
		task_active_workers++;
		rc = cape_dyn_send_run(w, t);
		free(t);
		if (rc != 0)
			return rc;
	}
	return 0;
}

/* Service workers blocked in taskwait: DONE once their children finished,
 * else hand them a queued task to run inline (deadlock avoidance). */
static int cape_dyn_release_waiters(void)
{
	int w, rc;

	for (w = 1; w < num_nodes; ++w) {
		long top;
		if (!dyn_waiting[w])
			continue;
		if (dyn_stk_n[w] <= 0) {   /* defensive: waiter with no task */
			dyn_waiting[w] = 0;
			rc = cape_dyn_send_done(w);
			if (rc != 0)
				return rc;
			continue;
		}
		top = dyn_stk[w][dyn_stk_n[w] - 1];
		if (dyn_kids[top] == 0) {
			dyn_waiting[w] = 0;
			rc = cape_dyn_send_done(w);
			if (rc != 0)
				return rc;
		} else if (dyn_q_len > 0) {
			struct cape_dyn_task *t = cape_dyn_dequeue();
			dyn_waiting[w] = 0;
			rc = cape_dyn_send_run(w, t);
			free(t);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int cape_dyn_handle_complete(int worker, unsigned char *payload,
				    size_t size)
{
	long tid, parent;
	int rc;

	if (worker <= 0 || worker >= num_nodes || dyn_stk_n[worker] <= 0) {
		fprintf(stderr,
			"Monitor %ld: dyn completion from unexpected worker %d\n",
			node, worker);
		return 1;
	}
	rc = cape_dyn_merge_bytes(payload + 1, size - 1);
	if (rc != 0)
		return rc;

	tid = dyn_stk[worker][--dyn_stk_n[worker]];
	parent = dyn_parent_of[tid];
	if (parent < 0)
		dyn_root_children--;
	else
		dyn_kids[parent]--;
	task_completed_jobs++;
	CAPE_DBG("dyn complete worker=%d id=%ld parent=%ld root_left=%ld",
		 worker, tid, parent, dyn_root_children);

	if (dyn_stk_n[worker] == 0) {
		task_worker_busy[worker] = 0;
		task_idle_workers[task_idle_count++] = worker;
		task_active_workers--;
	}
	rc = cape_dyn_try_dispatch();
	if (rc == 0)
		rc = cape_dyn_release_waiters();
	return rc;
}

static int cape_dyn_handle_submit(int worker, unsigned char *payload,
				  size_t size)
{
	unsigned long fn, args_size;
	long parent;
	int rc;

	if (worker <= 0 || worker >= num_nodes || dyn_stk_n[worker] <= 0) {
		fprintf(stderr,
			"Monitor %ld: dyn submit from unexpected worker %d\n",
			node, worker);
		return 1;
	}
	if (size < 1 + CAPE_DYN_DESC_HDR)
		return 1;
	memcpy(&fn, payload + 1, sizeof(unsigned long));
	memcpy(&args_size, payload + 1 + sizeof(unsigned long),
	       sizeof(unsigned long));
	if (args_size > DICKPT_TASK_ARGS_MAX ||
	    size < 1 + CAPE_DYN_DESC_HDR + args_size)
		return 1;

	/* The submitter's delta carries the parent's writes-so-far: the new
	 * task's inputs must be in total before it can be dispatched. */
	rc = cape_dyn_merge_bytes(payload + 1 + CAPE_DYN_DESC_HDR + args_size,
				  size - 1 - CAPE_DYN_DESC_HDR - args_size);
	if (rc != 0)
		return rc;

	parent = dyn_stk[worker][dyn_stk_n[worker] - 1];
	cape_dyn_enqueue(parent, fn, args_size,
			 payload + 1 + CAPE_DYN_DESC_HDR);
	rc = cape_dyn_try_dispatch();
	if (rc == 0)
		rc = cape_dyn_release_waiters();
	return rc;
}

static int cape_dyn_handle_wait(int worker)
{
	if (worker <= 0 || worker >= num_nodes)
		return 1;
	CAPE_DBG("dyn wait from worker=%d", worker);
	dyn_waiting[worker] = 1;
	return cape_dyn_release_waiters();
}

/* Master event pump. Processes every pending control message; if block is
 * set and nothing was pending, waits until at least one message arrives. */
static int cape_dyn_pump(int block)
{
	int handled = 0;

	for (;;) {
		ucp_tag_recv_info_t info;
		ucp_tag_message_h msg;
		int rc = cape_dyn_try_dispatch();

		if (rc != 0)
			return rc;
		msg = ucp_tag_probe_nb(ucp_worker,
				       CAPE_UCX_TAG(0, TAG_TASK_CTRL),
				       CAPE_UCX_TAG_TOKEN_MASK, 1, &info);
		if (msg != NULL) {
			size_t len = 0;
			int sender = -1;
			unsigned char *p = cape_ucx_recv_matched_alloc(msg,
								       &info,
								       &len,
								       &sender);
			if (len < 1) {
				free(p);
				return 1;
			}
			switch (p[0]) {
			case CAPE_TCTL_COMPLETE:
				rc = cape_dyn_handle_complete(sender, p, len);
				break;
			case CAPE_TCTL_SUBMIT:
				rc = cape_dyn_handle_submit(sender, p, len);
				break;
			case CAPE_TCTL_WAIT:
				rc = cape_dyn_handle_wait(sender);
				break;
			default:
				fprintf(stderr,
					"Monitor %ld: unknown task ctrl op %u\n",
					node, (unsigned)p[0]);
				rc = 1;
			}
			free(p);
			if (rc != 0)
				return rc;
			handled = 1;
			continue;
		}
		if (!block || handled)
			return 0;
		ucp_worker_progress(ucp_worker);
	}
}

/* Master: fold the just-generated final_ckpt into total_ckpt. Copy to the
 * heap first: merge_external_checkpoint would otherwise alias the scratch
 * buffer that the next generate_checkpoint reuses. */
static int cape_dyn_fold_final_into_total(void)
{
	unsigned char *buf;
	size_t n;
	int rc;

	if (final_ckpt_stream != NULL)
		fflush(final_ckpt_stream);
	n = final_ckpt_size;
	if (final_ckpt == NULL || n == 0) {
		release_final_checkpoint();
		return 0;
	}
	buf = malloc(n);
	if (buf == NULL)
		return 1;
	memcpy(buf, final_ckpt, n);
	release_final_checkpoint();
	rc = cape_dyn_merge_bytes(buf, n);
	free(buf);
	return rc;
}

/* Worker: inject a checkpoint byte range into the app. */
static int cape_dyn_inject_buf(unsigned char *data, size_t size)
{
	FILE *s;
	size_t sz = size;

	if (size == 0)
		return 0;
	s = fmemopen(data, size, "rb");
	if (s == NULL)
		return 1;
	return inject_checkpoint_with_write_access(s, &sz, &save_regs);
}

/* Read the app-side dickpt_task_desc at desc_addr. */
static int cape_dyn_read_desc(unsigned long desc_addr, unsigned long *fn,
			      unsigned long *args_size, unsigned char *args)
{
	unsigned long hdr[2];

	if (read_remote_memory(child_id, desc_addr, hdr, sizeof(hdr)) != 0)
		return 1;
	*fn = hdr[0];
	*args_size = hdr[1];
	if (*args_size > DICKPT_TASK_ARGS_MAX)
		return 1;
	if (*args_size != 0 &&
	    read_remote_memory(child_id, desc_addr + sizeof(hdr), args,
			       *args_size) != 0)
		return 1;
	return 0;
}

/* Worker: block for the next master directive; on RUN inject the shipped
 * checkpoint and copy the descriptor into the app's buffer at desc_addr.
 * Replies 1 (run the desc) or 0 (done/shutdown) to the app via rdx. */
static int cape_dyn_worker_directive(unsigned long desc_addr)
{
	unsigned char *p;
	size_t n = 0;
	unsigned long reply = 0;
	int rc = 0;

	p = cape_ucx_recv_probe_alloc(&n, 0, TAG_CKPT_DATA);
	if (n < 1) {
		free(p);
		return 1;
	}
	switch (p[0]) {
	case CAPE_TASK_RUN_DYN: {
		unsigned long args_size;
		size_t hdr;

		if (n < 1 + CAPE_DYN_DESC_HDR) { rc = 1; break; }
		memcpy(&args_size, p + 1 + sizeof(unsigned long),
		       sizeof(unsigned long));
		hdr = 1 + CAPE_DYN_DESC_HDR + args_size;
		if (args_size > DICKPT_TASK_ARGS_MAX || n < hdr) { rc = 1; break; }
		rc = cape_dyn_inject_buf(p + hdr, n - hdr);
		if (rc != 0)
			break;
		if (write_remote_memory(child_id, p + 1, desc_addr,
					CAPE_DYN_DESC_HDR + args_size) != 0) {
			rc = 1;
			break;
		}
		reply = 1;
		break;
	}
	case CAPE_TASK_DONE:
		rc = cape_dyn_inject_buf(p + 1, n - 1);
		reply = 0;
		break;
	case CAPE_TASK_SHUTDOWN:
		reply = 0;
		break;
	default:
		fprintf(stderr, "Monitor %ld: unknown dyn directive %u\n",
			node, (unsigned)p[0]);
		rc = 1;
	}
	free(p);
	if (rc == 0)
		rc = send_int_value_to_child((int)reply);
	CAPE_DBG("dyn directive handled reply=%lu rc=%d", reply, rc);
	return rc;
}

int require_task_spawn(unsigned long desc_addr)
{
	unsigned long fn = 0, args_size = 0;
	unsigned char args[DICKPT_TASK_ARGS_MAX];
	int rc;

	if (cape_dyn_read_desc(desc_addr, &fn, &args_size, args) != 0) {
		fprintf(stderr, "Monitor %ld: bad task descriptor\n", node);
		return 1;
	}
	CAPE_DBG("task_spawn node=%ld fn=0x%lx args=%lu", node, fn, args_size);

	if (node == 0) {
		if (cape_dyn_init() != 0)
			return 1;
		/* Capture the master's delta so the task observes everything
		 * written before its spawn point. */
		rc = require_generate_checkpoint();
		if (rc == 0)
			rc = cape_dyn_fold_final_into_total();
		if (rc != 0)
			return rc;
		cape_dyn_enqueue(-1, fn, args_size, args);
		return cape_dyn_pump(0);
	}

	/* Worker: snapshot the parent's writes-so-far and submit. */
	rc = require_generate_checkpoint();
	if (rc != 0)
		return rc;
	if (final_ckpt_stream != NULL)
		fflush(final_ckpt_stream);
	{
		size_t cn = final_ckpt_size;
		size_t mlen = 1 + CAPE_DYN_DESC_HDR + args_size + cn;
		unsigned char *msg = malloc(mlen);

		if (msg == NULL)
			return 1;
		msg[0] = CAPE_TCTL_SUBMIT;
		memcpy(msg + 1, &fn, sizeof(unsigned long));
		memcpy(msg + 1 + sizeof(unsigned long), &args_size,
		       sizeof(unsigned long));
		memcpy(msg + 1 + CAPE_DYN_DESC_HDR, args, args_size);
		if (cn != 0)
			memcpy(msg + 1 + CAPE_DYN_DESC_HDR + args_size,
			       final_ckpt, cn);
		cape_ucx_send(msg, mlen, 0, TAG_TASK_CTRL);
		free(msg);
	}
	release_final_checkpoint();
	return 0;
}

int require_task_serve(unsigned long desc_addr)
{
	if (node == 0)
		return send_int_value_to_child(0);
	return cape_dyn_worker_directive(desc_addr);
}

int require_task_wait(unsigned long desc_addr)
{
	if (node == 0) {
		int rc = 0;
		if (!dyn_ready)
			return send_int_value_to_child(0);
		while (dyn_root_children > 0) {
			rc = cape_dyn_pump(1);
			if (rc != 0)
				return rc;
		}
		/* Make the children's merged outputs visible to the master app
		 * (taskwait semantics) without releasing the accumulator. */
		{
			const unsigned char *tot;
			size_t tn;
			cape_dyn_total_view(&tot, &tn);
			if (tn != 0 &&
			    cape_dyn_inject_buf((unsigned char *)tot, tn) != 0)
				return 1;
		}
		return send_int_value_to_child(0);
	}
	{
		unsigned char op = CAPE_TCTL_WAIT;
		cape_ucx_send(&op, sizeof(op), 0, TAG_TASK_CTRL);
	}
	return cape_dyn_worker_directive(desc_addr);
}

int require_task_complete(void)
{
	size_t cn, mlen;
	unsigned char *msg;

	if (node == 0)
		return 0;
	if (final_ckpt_stream != NULL)
		fflush(final_ckpt_stream);
	cn = (final_ckpt != NULL) ? final_ckpt_size : 0;
	mlen = 1 + cn;
	msg = malloc(mlen);
	if (msg == NULL)
		return 1;
	msg[0] = CAPE_TCTL_COMPLETE;
	if (cn != 0)
		memcpy(msg + 1, final_ckpt, cn);
	cape_ucx_send(msg, mlen, 0, TAG_TASK_CTRL);
	free(msg);
	release_final_checkpoint();
	return 0;
}

int require_task_region_end(void)
{
	int w, rc;
	unsigned char shutdown_msg = CAPE_TASK_SHUTDOWN;

	if (node != 0)
		return 0;
	if (cape_dyn_init() != 0)
		return 1;
	while (task_active_workers > 0 || dyn_q_len > 0) {
		rc = cape_dyn_pump(1);
		if (rc != 0)
			return rc;
	}
	for (w = 1; w < num_nodes; ++w)
		cape_ucx_send(&shutdown_msg, sizeof(shutdown_msg), w,
			      TAG_CKPT_DATA);
	CAPE_DBG("dyn region end: %lu tasks completed", task_completed_jobs);
	/* Region-local bookkeeping resets; the worker pool stays ready. */
	dyn_root_children = 0;
	dyn_next_id = 0;
	return 0;
}

/* ==========================================================================
 * critical / atomic: rank-serialized section with checkpoint chaining.
 * Rank 0 runs first; rank k>0 blocks in ENTER until rank k-1's EXIT ships
 * the accumulated section state (injected on receipt). EXIT folds this
 * rank's delta into the state, forwards it down the chain, and the last
 * rank broadcasts the final state so every rank leaves the section with
 * identical shared memory.
 * ==========================================================================
 */
static unsigned char *crit_in;        /* predecessor's accumulated state */
static size_t crit_in_size;

/* Merge two checkpoints into a fresh heap buffer; delta wins conflicts.
 * Both must share the standard [timespan][regs][BMP section] layout. */
static unsigned char *cape_merge_ckpt_pair(unsigned char *base, size_t base_n,
					   unsigned char *delta, size_t delta_n,
					   size_t *out_n)
{
	FILE *out;
	unsigned char *out_buf = NULL;
	struct bmp_section_view base_bmp, delta_bmp;
	size_t payload_pos = sizeof(unsigned long) +
			     sizeof(struct user_regs_struct);
	int rc = 1;

	*out_n = 0;
	if (base_n < payload_pos || delta_n < payload_pos)
		return NULL;
	memset(&base_bmp, 0, sizeof(base_bmp));
	memset(&delta_bmp, 0, sizeof(delta_bmp));
	out = open_binary_memstream(&out_buf, out_n);
	if (out == NULL)
		return NULL;
	if (parse_bitmap_section(base, base_n, payload_pos, &base_bmp) == 0 &&
	    parse_bitmap_section(delta, delta_n, payload_pos, &delta_bmp) == 0 &&
	    fwrite(delta, payload_pos, 1, out) == 1 &&
	    merge_bitmap_sections(out, &base_bmp, &delta_bmp) == 0)
		rc = 0;
	free_bitmap_section(&base_bmp);
	free_bitmap_section(&delta_bmp);
	fclose(out);
	if (rc != 0) {
		free(out_buf);
		return NULL;
	}
	return out_buf;
}

int require_critical_enter(void)
{
	if (num_nodes <= 1 || node == 0)
		return 0;
	free(crit_in);
	crit_in = NULL;
	crit_in_size = 0;
	crit_in = cape_ucx_recv_probe_alloc(&crit_in_size, (int)node - 1,
					    TAG_CRIT_CHAIN);
	CAPE_DBG("critical_enter got state size=%zu", crit_in_size);
	if (crit_in_size == 0) {
		free(crit_in);
		crit_in = NULL;
		return 1;
	}
	return cape_dyn_inject_buf(crit_in, crit_in_size);
}

int require_critical_exit(void)
{
	unsigned char *state = NULL;
	size_t state_n = 0;
	int rc;

	if (num_nodes <= 1)
		return 0;

	/* This rank's section delta. */
	rc = require_generate_checkpoint();
	if (rc != 0)
		return rc;
	if (final_ckpt_stream != NULL)
		fflush(final_ckpt_stream);
	if (final_ckpt == NULL || final_ckpt_size == 0) {
		release_final_checkpoint();
		return 1;
	}

	if (crit_in != NULL) {
		state = cape_merge_ckpt_pair(crit_in, crit_in_size,
					     final_ckpt, final_ckpt_size,
					     &state_n);
		free(crit_in);
		crit_in = NULL;
		crit_in_size = 0;
	} else {
		state = malloc(final_ckpt_size);
		if (state != NULL) {
			memcpy(state, final_ckpt, final_ckpt_size);
			state_n = final_ckpt_size;
		}
	}
	release_final_checkpoint();
	if (state == NULL)
		return 1;
	CAPE_DBG("critical_exit state size=%zu", state_n);

	if ((int)node < num_nodes - 1) {
		cape_ucx_send(state, state_n, (int)node + 1, TAG_CRIT_CHAIN);
		free(state);
		/* Adopt the final section state from the last rank. */
		state = cape_ucx_recv_probe_alloc(&state_n, num_nodes - 1,
						  TAG_CRIT_FINAL);
		rc = cape_dyn_inject_buf(state, state_n);
		free(state);
		return rc;
	}

	/* Last rank already holds the final state in memory: publish it. */
	{
		int i;
		for (i = 0; i < num_nodes - 1; ++i)
			cape_ucx_send(state, state_n, i, TAG_CRIT_FINAL);
	}
	free(state);
	return 0;
}

	/* ----------------------------------------
	 * receive_checkpoint(): receive final checkpoint and save into a memory stream file
 * ----------------------------------------
 */
 FILE * receive_checkpoint(int source, unsigned char **ckpt_data, size_t *ckpt_size) {
	unsigned char *buffer;
	size_t nbytes;
	FILE *stream;

	buffer = cape_ucx_recv_probe_alloc(&nbytes, source, TAG_CKPT_DATA);

	//open the stream memory file
	stream = open_binary_memstream(ckpt_data, ckpt_size);
	fwrite(buffer, nbytes, 1, stream);
	fflush(stream);
	free(buffer);

	return stream;

 } 
/* -------------------------------------------------------------
 * require_receive_checkpoint(): receive checkpoint * 	
 * 	Slave: save into after_ckpt
 * 	For master, will be implemented at waitfor_checkpoint()
 * -------------------------------------------------------------
 */
 int require_receive_checkpoint() {
 	int rc = 0;
 	if(node!=0){  		
 		//receive_after_checkpoint(0); 		
 		after_ckpt_stream = receive_checkpoint(0, &after_ckpt, &after_ckpt_size);
 		
 		cape_ucx_recv(&ckpt_flag, sizeof(int), 0, TAG_CKPT_FLAG); 		
 		//printf("\nMonitor %ld: Checkpoint received, file size: %d  - ckpt_flag=%d \n", 
 		//	node, after_ckpt_size, ckpt_flag);
 	}
	if (rc!=0) printf("Monitor %ld: Error on require_receive_checkpoint \n", node);
 	return rc; 	
 }

static int inject_bitmap_section_from_stream(FILE *stream, size_t file_size,
					     unsigned long *file_pointer)
{
	uint32_t n_pages, i;
	uint64_t *addrs = NULL;
	int rc = 0;

	if (fread(&n_pages, sizeof(n_pages), 1, stream) != 1)
		return 1;
	*file_pointer += sizeof(n_pages);

	if (n_pages == 0)
		return 0;

	if (*file_pointer > file_size ||
	    (size_t)n_pages * sizeof(uint64_t) > file_size - *file_pointer)
		return 1;

	addrs = malloc((size_t)n_pages * sizeof(uint64_t));
	if (addrs == NULL)
		return 1;
	if (fread(addrs, sizeof(uint64_t), n_pages, stream) != n_pages) {
		rc = 1;
		goto out;
	}
	*file_pointer += (size_t)n_pages * sizeof(uint64_t);

	for (i = 0; i < n_pages; ++i) {
		uint8_t word_bmp[BMP_WORD_BMP_BYTES];
		uint32_t live[BMP_WORDS_PER_PAGE];
		uint32_t *changed = NULL;
		unsigned w, k = 0, n_changed;
		size_t payload_bytes;

		if (*file_pointer > file_size ||
		    BMP_WORD_BMP_BYTES > file_size - *file_pointer) {
			rc = 1; goto out;
		}
		if (fread(word_bmp, BMP_WORD_BMP_BYTES, 1, stream) != 1) {
			rc = 1; goto out;
		}
		*file_pointer += BMP_WORD_BMP_BYTES;

		n_changed = wbmp_popcount(word_bmp);
		payload_bytes = (size_t)n_changed * sizeof(uint32_t);
		if (*file_pointer > file_size ||
		    payload_bytes > file_size - *file_pointer) {
			rc = 1; goto out;
		}
		if (n_changed != 0) {
			changed = malloc(payload_bytes);
			if (changed == NULL) { rc = 1; goto out; }
			if (fread(changed, payload_bytes, 1, stream) != 1) {
				free(changed); rc = 1; goto out;
			}
		}
		*file_pointer += payload_bytes;

		if (read_remote_memory(child_id, (unsigned long)addrs[i],
				       live, BMP_PAGE_SIZE) != 0) {
			fprintf(stderr,
				"Monitor %ld: failed to read live page at 0x%lx\n",
				node, (unsigned long)addrs[i]);
			free(changed); rc = 1; goto out;
		}
		for (w = 0; w < BMP_WORDS_PER_PAGE; ++w) {
			if (wbmp_get(word_bmp, w)) {
				if (k >= n_changed) {
					free(changed); rc = 1; goto out;
				}
				live[w] = changed[k++];
			}
		}
		if (write_remote_memory(child_id, live,
					(unsigned long)addrs[i],
					BMP_PAGE_SIZE) != 0) {
			fprintf(stderr,
				"Monitor %ld: failed to inject page at 0x%lx: %s\n",
				node, (unsigned long)addrs[i], strerror(errno));
			free(changed); rc = 1; goto out;
		}
		free(changed);
	}

out:
	free(addrs);
	return rc;
}
 
/*------------------------------------
 * inject_checkpoint(): inject checkpoint into memory of CAPE program
 * 	Checkpoint manage by final_ckpt_stream, final_ckpt and final_ckpt_size
 * Memory file will be closed after injected.
 * -----------------------------------
 */
int inject_checkpoint(FILE *stream, size_t *file_size, struct user_regs_struct * regs){
	unsigned long marker, ts;
	unsigned long file_pointer = 0;
	int rc = 0;

	ptrace(PTRACE_GETREGS, child_id, NULL, regs);

	fseek(stream, 0, SEEK_SET);
	fread(&ts, sizeof(unsigned long), 1, stream);
	file_pointer = sizeof(unsigned long);
	fseek(stream, file_pointer, SEEK_SET);
	fread(regs, sizeof(struct user_regs_struct), 1, stream);
	file_pointer += sizeof(struct user_regs_struct);
	fseek(stream, file_pointer, SEEK_SET);

	while (file_pointer < *file_size) {
		if (fread(&marker, sizeof(marker), 1, stream) != 1) {
			rc = 1;
			break;
		}
		file_pointer += sizeof(marker);
		if (marker != BMP_S) {
			fprintf(stderr,
				"Monitor %ld: unexpected ckpt marker %lu at offset %lu\n",
				node, marker, file_pointer - sizeof(marker));
			rc = 1;
			break;
		}
		rc = inject_bitmap_section_from_stream(stream, *file_size,
						       &file_pointer);
		if (rc != 0)
			break;
		fseek(stream, file_pointer, SEEK_SET);
	}

	fclose(stream);
	*file_size = 0;
	return rc;
}

static int inject_checkpoint_with_write_access(FILE *stream, size_t *file_size,
		struct user_regs_struct *regs)
{
	int rc;
	int wp_rc;
	int reprotect = 0;

	if (tracking_is_enabled) {
		rc = cape_writeprotect_tracked_ranges(0);
		if (rc != 0)
			return rc;
		reprotect = 1;
	}

	rc = inject_checkpoint(stream, file_size, regs);

	if (reprotect) {
		wp_rc = cape_writeprotect_tracked_ranges(1);
		if (rc == 0)
			rc = wp_rc;
	}

	return rc;
}
  

int require_inject_checkpoint()
{
	FILE *inject_stream = NULL;
	size_t inject_size = 0;
	int rc = 0;
	CAPE_DBG("require_inject_checkpoint enter final_size=%zu total_size=%zu after_size=%zu",
		 final_ckpt_size, total_ckpt_size, after_ckpt_size);

	if (node != 0) {
		if (after_ckpt_stream != NULL) {
			fflush(after_ckpt_stream);
			fclose(after_ckpt_stream);
			after_ckpt_stream = NULL;
		}
		if (after_ckpt == NULL || after_ckpt_size == 0)
			return 0;
		inject_stream = fmemopen(after_ckpt, after_ckpt_size, "rb");
		if (inject_stream == NULL)
			return 1;
		inject_size = after_ckpt_size;
		rc = inject_checkpoint_with_write_access(inject_stream, &inject_size,
							 &save_regs);
		free(after_ckpt);
		after_ckpt = NULL;
		after_ckpt_size = 0;
		CAPE_DBG("require_inject_checkpoint worker exit rc=%d", rc);
		return rc;
	}

	if (total_ckpt_stream != NULL) {
		if (cape_total_stream_update_size() != 0)
			return 1;
		fclose(total_ckpt_stream);
		total_ckpt_stream = NULL;
	}

	if (total_ckpt != NULL && total_ckpt_size > 0) {
		inject_stream = fmemopen(total_ckpt, total_ckpt_size, "rb");
		if (inject_stream == NULL)
			return 1;
		inject_size = total_ckpt_size;
		rc = inject_checkpoint_with_write_access(inject_stream, &inject_size,
							 &save_regs);
		cape_total_release();
		CAPE_DBG("require_inject_checkpoint master total exit rc=%d", rc);
		return rc;
	}

	if (final_ckpt_stream != NULL) {
		fflush(final_ckpt_stream);
		fclose(final_ckpt_stream);
		final_ckpt_stream = NULL;
	}
	if (final_ckpt == NULL || final_ckpt_size == 0)
		return 0;
	inject_stream = fmemopen(final_ckpt, final_ckpt_size, "rb");
	if (inject_stream == NULL)
		return 1;
	inject_size = final_ckpt_size;
	rc = inject_checkpoint_with_write_access(inject_stream, &inject_size,
						 &save_regs);
	release_final_checkpoint();
	CAPE_DBG("require_inject_checkpoint master final exit rc=%d", rc);
	return rc;
}



 
int merge_external_checkpoint(FILE *src_ckpt_stream, 		\
							  unsigned char *src_ckpt_data, \
							  size_t src_ckpt_size 	)
 {
	FILE *tmp_read_stream = NULL, *src_read_stream = NULL;
	unsigned char *tmp_ckpt;
	size_t tmp_size;
	struct bmp_section_view tmp_bmp = {0}, src_bmp = {0};
	unsigned char *old_total;
	int old_total_was_scratch;
	CAPE_PROFILE_NS_VAR(merge_start_ns);
	CAPE_PROFILE_NS_START(merge_start_ns);

 	size_t payload_pos;
 	int rc =0;
 	unsigned long t1, t2;

 	if (src_ckpt_size == 0 ) return 1;

 	if(total_ckpt_size==0)
 	{
		if (cape_scratch_memh_for(src_ckpt_data) != NULL) {
			total_ckpt = src_ckpt_data;
			total_ckpt_size = src_ckpt_size;
			total_ckpt_uses_scratch = 1;
			total_ckpt_stream = NULL;
			return 0;
		}
		total_ckpt_stream = cape_total_open_scratch_stream(src_ckpt_size);
		if (total_ckpt_stream == NULL)
			return 1;
		if (fwrite(src_ckpt_data, src_ckpt_size, 1, total_ckpt_stream) != 1 ||
		    cape_total_stream_update_size() != 0)
			return 1;
		return 0;
 	}

	if (cape_total_stream_update_size() != 0)
		return 1;
	old_total = total_ckpt;
	old_total_was_scratch = total_ckpt_uses_scratch;
	tmp_size = total_ckpt_size;

	if (old_total_was_scratch) {
		tmp_ckpt = old_total;
	} else {
		if (cape_copy_to_scratch(&ucx_scratch_total_in,
					 &ucx_scratch_total_in_cap,
					 old_total, tmp_size) != 0)
			return 1;
		tmp_ckpt = ucx_scratch_total_in;
	}

	if (total_ckpt_stream != NULL) {
		fclose(total_ckpt_stream);
		total_ckpt_stream = NULL;
	}
	if (!old_total_was_scratch)
		free(old_total);
	total_ckpt = tmp_ckpt;
	total_ckpt_uses_scratch = 1;
	total_ckpt_size = 0;

	tmp_read_stream = fmemopen(tmp_ckpt, tmp_size, "rb");
	src_read_stream = fmemopen(src_ckpt_data, src_ckpt_size, "rb");
	if (tmp_read_stream == NULL || src_read_stream == NULL) {
		rc = 1;
		goto done;
	}

 	//1. Read timespan
	if (fread(&t1, sizeof(unsigned long), 1, tmp_read_stream) != 1 ||
	    fread(&t2, sizeof(unsigned long), 1, src_read_stream) != 1) {
		rc = 1;
		goto done;
	}

	if (tmp_size > SIZE_MAX - src_ckpt_size ||
	    tmp_size + src_ckpt_size > SIZE_MAX - 4096) {
		rc = 1;
		goto done;
	}
	total_ckpt_stream = cape_total_open_scratch_stream(tmp_size + src_ckpt_size + 4096);
	if (total_ckpt_stream == NULL) {
		rc = 1;
		goto done;
	}

	payload_pos = sizeof(unsigned long) + sizeof(struct user_regs_struct);
	memset(&tmp_bmp, 0, sizeof(tmp_bmp));
	memset(&src_bmp, 0, sizeof(src_bmp));
	if (parse_bitmap_section(tmp_ckpt, tmp_size, payload_pos, &tmp_bmp) != 0 ||
	    parse_bitmap_section(src_ckpt_data, src_ckpt_size, payload_pos, &src_bmp) != 0) {
		rc = 1;
		goto done;
	}

 	if (t1 >= t2)
 	{
		//Write t1 and R1 into Total_ckpt
		fwrite(tmp_ckpt, sizeof(unsigned long) + sizeof(struct user_regs_struct), 1, total_ckpt_stream);
		fflush(total_ckpt_stream);

		/* Reduction-aware bitmap merge subsumes the old L section:
		 * dirty words at reduction-variable addresses get combined
		 * here, the rest follow newer-wins. No L data is emitted. */
		rc = merge_bitmap_sections(total_ckpt_stream, &src_bmp, &tmp_bmp);
		if (rc != 0)
			goto done;
	}
 	else
 	{
		fwrite(src_ckpt_data, sizeof(unsigned long) + sizeof(struct user_regs_struct), 1, total_ckpt_stream);
		fflush(total_ckpt_stream);

		rc = merge_bitmap_sections(total_ckpt_stream, &tmp_bmp, &src_bmp);
		if (rc != 0)
			goto done;
	}

done:
	free_bitmap_section(&tmp_bmp);
	free_bitmap_section(&src_bmp);
	if (tmp_read_stream != NULL)
		fclose(tmp_read_stream);
	if (src_read_stream != NULL)
		fclose(src_read_stream);

	if (total_ckpt_stream != NULL && cape_total_stream_update_size() != 0)
		rc = 1;

	CAPE_PROFILE_ADD_NS(merge_ext_ns, merge_start_ns);
	CAPE_PROFILE_INC(merge_ext_calls);
 	return rc;
 }
 

 int require_waitfor_checkpoint() {
 	int rc = 0, i;
	CAPE_DBG("require_waitfor_checkpoint enter task_pool_ready=%d active=%d",
		 task_pool_ready, task_active_workers);

	if (node == 0 && task_pool_ready) {
		while (task_active_workers > 0) {
			CAPE_DBG("waitfor waiting active=%d completed=%lu dispatched=%lu",
				 task_active_workers, task_completed_jobs,
				 task_dispatched_jobs);
			rc = cape_task_wait_for_completion();
			if (rc != 0)
				break;
		}
	} else if (node == 0) {
		for (i = 1; i < num_nodes; i++) {
			CAPE_DBG("waitfor receiving checkpoint from rank=%d", i);
			after_ckpt_stream = receive_checkpoint(i, &after_ckpt, &after_ckpt_size);
			rc = merge_external_checkpoint(after_ckpt_stream, after_ckpt,
						       after_ckpt_size);
			fclose(after_ckpt_stream);
			free(after_ckpt);
			after_ckpt = NULL;
			after_ckpt_size = 0;
			if (rc != 0)
				break;
		}
	}

	dprintf("Monitor %ld: waitfor drained tasks dispatched=%lu completed=%lu active=%d\n",
		node, task_dispatched_jobs, task_completed_jobs, task_active_workers);

	if (rc != 0)
		printf("Monitor %ld: Error on require_waitfor_checkpoint\n", node);
	CAPE_DBG("require_waitfor_checkpoint exit rc=%d active=%d completed=%lu",
		 rc, task_active_workers, task_completed_jobs);
	return rc;
 }
 /* -----------------------------------------------------------------
  * require_broadcast_checkpoint(): Synchronize data between node
  * 	broadcast final_checkpoint  	
  * -----------------------------------------------------------------
  */
int require_broadcast_checkpoint(){
	int rc = 0;
	int i;
	unsigned char *src_ckpt = NULL;
	size_t src_ckpt_size = 0;
	CAPE_DBG("require_broadcast_checkpoint enter");
	if(node==0)
	{
		if (total_ckpt_stream != NULL && cape_total_stream_update_size() != 0)
			return 1;
		if (total_ckpt != NULL && total_ckpt_size > 0) {
			src_ckpt = total_ckpt;
			src_ckpt_size = total_ckpt_size;
		} else {
			src_ckpt = final_ckpt;
			src_ckpt_size = final_ckpt_size;
		}
		buffer_ckpt = src_ckpt;
		/* Master sends size and data to all slaves */
		for(i = 1; i < num_nodes; i++){
			CAPE_DBG("broadcast send rank=%d size=%zu", i, src_ckpt_size);
			cape_ucx_send(buffer_ckpt, src_ckpt_size, i, TAG_BCAST_DATA);
		}
	}
	else
	{
		size_t recv_size;
		CAPE_DBG("broadcast recv begin");
		buffer_ckpt = cape_ucx_recv_probe_alloc(&recv_size, 0, TAG_BCAST_DATA);
		CAPE_DBG("broadcast recv got size=%zu", recv_size);
		if (recv_size > (size_t)INT_MAX) {
			free(buffer_ckpt);
			return 1;
		}
		buffer_size = (int)recv_size;
		after_ckpt_stream = open_binary_memstream(&after_ckpt, &after_ckpt_size);
		fwrite(buffer_ckpt, recv_size, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		free(buffer_ckpt);
	}

	CAPE_DBG("require_broadcast_checkpoint exit rc=%d", rc);
	return rc;
}
 

/*----------------------------------------------------------------------
 * Check a number is power of 2 or not
 *---------------------------------------------------------------------
 */
int is_power_of_two(unsigned int n)
{
  if (n == 0) return 0;
  while (n != 1){
    if (n%2 != 0) return 0;
    n = n/2;
  }
  return 1;
}

/*----------------------------------------------------------------------
 * log2(n): calculate log2 of n
 *----------------------------------------------------------------------
 */
int mylog2(unsigned int n){
	int p = 0;
	int tmp;
	tmp = n;
	while(tmp >>=1) ++p;
	return p;	
}

/*----------------------------------------------------------------------
 * ring_allreduce(): Allreduce using Ring algorithm
 * 
 * ---------------------------------------------------------------------
 */
int ring_allreduce(){
	int rc = 0;

	size_t message_size;
	size_t recv_message_size = 0;
	unsigned char *send_buffer;
	unsigned char *recv_buffer;
	size_t recv_cap;
	FILE *ckpt_stream;

	int left;
	int right;
	int i;

	send_buffer = total_ckpt;
	message_size = total_ckpt_size;
	recv_buffer = ucx_scratch_recv;
	recv_cap = ucx_scratch_recv_cap;

	left = ( node - 1 + num_nodes ) % num_nodes;
	right = (node + 1 ) % num_nodes ;

	for(i = 1 ; i < num_nodes; i++){
		uint32_t token_data = TAG_ALLREDUCE_BASE + (i * 2) + 1;
		CAPE_PROFILE_NS_VAR(step_start_ns);
		CAPE_PROFILE_NS_START(step_start_ns);

		recv_message_size = cape_ucx_sendrecv_probe(send_buffer,
							    message_size, right,
							    recv_buffer,
							    recv_cap, left,
							    token_data);

		ckpt_stream = fmemopen(recv_buffer, recv_message_size, "rb");
		if (ckpt_stream == NULL)
			return 1;
		rc = merge_external_checkpoint(ckpt_stream, recv_buffer,
					       recv_message_size);
		fclose(ckpt_stream);
		if (rc != 0)
			return rc;

		send_buffer = recv_buffer;
		message_size = recv_message_size;
		if (recv_buffer == ucx_scratch_recv) {
			recv_buffer = ucx_scratch_send;
			recv_cap = ucx_scratch_send_cap;
		} else {
			recv_buffer = ucx_scratch_recv;
			recv_cap = ucx_scratch_recv_cap;
		}

		CAPE_PROFILE_ADD_NS(allreduce_total_ns, step_start_ns);
		CAPE_PROFILE_INC(allreduce_steps);
	}

    return rc;
}


int hypercube_allreduce(){
	int rc = 0;
	int i;
	int nsteps = 0;
	int partner;
	size_t send_msg_size = 0, recv_msg_size = 0;

	FILE *ckpt_stream;

	nsteps = mylog2 (num_nodes);

	for(i = 0; i < nsteps; i ++){
		uint32_t token_data = TAG_ALLREDUCE_BASE + (i * 2) + 1;
		CAPE_PROFILE_NS_VAR(step_start_ns);
		CAPE_PROFILE_NS_START(step_start_ns);

		partner = node ^ (1 << i);

		send_msg_size = total_ckpt_size;
		recv_msg_size = cape_ucx_sendrecv_probe(total_ckpt,
							send_msg_size,
							partner,
							ucx_scratch_recv,
							ucx_scratch_recv_cap,
							partner,
							token_data);

		ckpt_stream = fmemopen(ucx_scratch_recv,
				       recv_msg_size, "rb");
		if (ckpt_stream == NULL)
			return 1;
		rc = merge_external_checkpoint(ckpt_stream, ucx_scratch_recv,
					       recv_msg_size);
		fclose(ckpt_stream);
		if (rc != 0)
			return rc;

		CAPE_PROFILE_ADD_NS(allreduce_total_ns, step_start_ns);
		CAPE_PROFILE_INC(allreduce_steps);
	}


    return rc;
}

int require_allreduce_checkpoint(){
	int rc = 0;
	rc=  merge_external_checkpoint(final_ckpt_stream, final_ckpt, final_ckpt_size);		
	fclose(final_ckpt_stream);
	if (!final_ckpt_uses_scratch)
		free(final_ckpt);
	final_ckpt = NULL;
	final_ckpt_size = 0;
	final_ckpt_uses_scratch = 0;
	if (rc != 0) {
		cape_total_release();
		return rc;
	}
		
	if (is_power_of_two(num_nodes))
		rc = hypercube_allreduce();
	else
		rc = ring_allreduce();
	if (rc != 0) {
		cape_total_release();
		return rc;
	}
	
	if (cape_total_stream_update_size() != 0) {
		cape_total_release();
		return 1;
	}
	cape_profile_record_merged_ckpt(total_ckpt_size);

	if (total_ckpt_stream != NULL) {
		fclose(total_ckpt_stream);
		total_ckpt_stream = NULL;
	}
	{
		FILE *inject_stream = fmemopen(total_ckpt, total_ckpt_size, "rb");
		size_t inject_size = total_ckpt_size;
		if (inject_stream == NULL) {
			cape_total_release();
			return 1;
		}
		rc = inject_checkpoint_with_write_access(inject_stream, &inject_size, &save_regs);
	}
	cape_total_release();

	return rc;
}
