#define _GNU_SOURCE


#include <assert.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include "../include/cape_monitor.h"
#include "../include/cape_dickpt_uffd.h"
#include "../include/cape_signal.h"

#include <ucp/api/ucp.h>
#ifdef USE_PMIX
#include <pmix.h>
#endif

/* OpenMP directive codes (mirror cape.h; defined here so cape_bitmap.c
 * does not need to include cape.h, which would clash with cape_monitor.h). */
#ifndef PARALLEL
#define PARALLEL      2
#define FOR           3
#define FOR_NOWAIT    4
#define PARALLEL_FOR  5
#define SECTIONS      6
#define MASTER        8
#define SINGLE        9
#endif

struct page_node * list_head = NULL, * list_end = NULL;


//list to manage share-data properties that is sent by application
struct shared_data * data_list_head = NULL;
struct shared_data * data_list_tail = NULL;

//list to manage incremental checkpoint of share-data attribute variables
struct shared_data_ckpt * list_ckpt_head = NULL;
struct shared_data_ckpt * list_ckpt_tail = NULL;

struct shared_data_ckpt * final_list_ckpt_head = NULL;
struct shared_data_ckpt * final_list_ckpt_tail = NULL;

int userfault_fd = -1;
static int epoll_fd = -1;
static pthread_t uffd_thread;
static pthread_mutex_t bitmap_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int uffd_thread_run = 0;
/* Legacy pid argument to the in-process ioctl_*_data wrappers; ignored. */
static const int child_id = 0;

struct cape_dickpt_range *tracked_ranges = NULL;
size_t tracked_range_count = 0;
int tracking_is_enabled = 0;

/* ===== bitmap S-section format =====
 *   [BMP_S:8] [n_ranges:4]
 *   For each tracked range:
 *     [base_addr:8] [n_pages:4] [n_dirty:4]
 *     [page_bmp: ceil(n_pages/8) bytes]
 *     [dirty_pages_data: n_dirty * 4096 bytes]   (page-aligned, in page_bmp scan order)
 *
 * Merging two S sections: union the page bitmaps; pages dirty on only
 * one side are bulk-copied; pages dirty on both sides resolve by
 * "newer wins" (the merge function picks the side at section level
 * via the timestamp comparison in merge_external_checkpoint).
 *
 * Inject: walk page_bmp, write each dirty page in one 4 KB memcpy.
 *
 * The L section that follows continues with the existing SD/MD/EP
 * markers unchanged, so reductions and shared-data still work. */
/* Word-granular delta format (ported from cape_incr_bitmap.c):
 *   [BMP_S:8] [n_dirty_pages:4]
 *   [page_addr_0:8] ... [page_addr_{n-1}:8]            // sorted ascending
 *   For each page i:
 *     [word_bmp_i:128]                                  // 1024 bits, 1 = word changed
 *     [changed_words_i: popcount(word_bmp_i) * 4 bytes] // packed in bit-scan order
 *
 * This replaces the old page-granular full-page format, which lost
 * disjoint sub-page writes from different ranks (each rank writing its
 * own quarter of a shared 4 KB page) at merge time. Word-level merge
 * unions per-word so data-parallel writes to one page combine correctly. */
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
	/* Manual byte popcount — avoids __builtin_popcount, which pulls
	 * _popcountsi2.o out of libgcc.a and trips the cluster's
	 * binutils/libgcc mismatch ("File format not recognized"). */
	static const uint8_t nbits[16] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};
	unsigned n = 0, i;
	for (i = 0; i < BMP_WORD_BMP_BYTES; ++i)
		n += nbits[b[i] & 0xf] + nbits[b[i] >> 4];
	return n;
}

/* A word at addr is checkpointed only if it lies inside a declared
 * shared region (data_list_head). Private/scalar words on a tracked page
 * are masked out, so they are never exchanged across ranks. */
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

struct shared_data *is_in_share_data_list(unsigned long int addr,
					  struct shared_data *list);
int add_item_to_list_ckpt(struct shared_data_ckpt *p);

static unsigned reduction_size(unsigned char dt)
{
	switch (dt) {
	case CAPE_CHAR: case CAPE_UNSIGNED_CHAR:
		return 1;
	case CAPE_SHORT: case CAPE_UNSIGNED_SHORT:
		return 2;
	case CAPE_INT: case CAPE_UNSIGNED_INT: case CAPE_FLOAT:
		return 4;
	case CAPE_LONG: case CAPE_UNSIGNED_LONG: case CAPE_DOUBLE:
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
		case CAPE_INT:           APPLY_BIN(int32_t,  +); break;
		case CAPE_UNSIGNED_INT:  APPLY_BIN(uint32_t, +); break;
		case CAPE_LONG:          APPLY_BIN(int64_t,  +); break;
		case CAPE_UNSIGNED_LONG: APPLY_BIN(uint64_t, +); break;
		case CAPE_FLOAT:         APPLY_BIN(float,    +); break;
		case CAPE_DOUBLE:        APPLY_BIN(double,   +); break;
		}
		break;
	case D_REDUCTION_MUL:
		switch (dt) {
		case CAPE_INT:           APPLY_BIN(int32_t,  *); break;
		case CAPE_UNSIGNED_INT:  APPLY_BIN(uint32_t, *); break;
		case CAPE_LONG:          APPLY_BIN(int64_t,  *); break;
		case CAPE_UNSIGNED_LONG: APPLY_BIN(uint64_t, *); break;
		case CAPE_FLOAT:         APPLY_BIN(float,    *); break;
		case CAPE_DOUBLE:        APPLY_BIN(double,   *); break;
		}
		break;
	case D_REDUCTION_MAX:
		switch (dt) {
		case CAPE_INT:           APPLY_MAX(int32_t);  break;
		case CAPE_UNSIGNED_INT:  APPLY_MAX(uint32_t); break;
		case CAPE_LONG:          APPLY_MAX(int64_t);  break;
		case CAPE_UNSIGNED_LONG: APPLY_MAX(uint64_t); break;
		case CAPE_FLOAT:         APPLY_MAX(float);    break;
		case CAPE_DOUBLE:        APPLY_MAX(double);   break;
		}
		break;
	case D_REDUCTION_MIN:
		switch (dt) {
		case CAPE_INT:           APPLY_MIN(int32_t);  break;
		case CAPE_UNSIGNED_INT:  APPLY_MIN(uint32_t); break;
		case CAPE_LONG:          APPLY_MIN(int64_t);  break;
		case CAPE_UNSIGNED_LONG: APPLY_MIN(uint64_t); break;
		case CAPE_FLOAT:         APPLY_MIN(float);    break;
		case CAPE_DOUBLE:        APPLY_MIN(double);   break;
		}
		break;
	case D_REDUCTION_AND: APPLY_BIN(uint64_t, &); break;
	case D_REDUCTION_OR:  APPLY_BIN(uint64_t, |); break;
	case D_REDUCTION_XOR: APPLY_BIN(uint64_t, ^); break;
	}
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

/* Word-level merge of one page present on both sides. Disjoint writes
 * union; words written by both sides resolve newer-wins, unless the word
 * is a declared reduction variable (then combine with its op). */
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
 * out verbatim. Pages on both sides go through emit_merged_page. */
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

static void collect_l_word(unsigned long addr, const void *word,
			   unsigned char cflag)
{
	struct shared_data *plist;
	struct shared_data_ckpt item;

	plist = is_in_share_data_list(addr, data_list_head);
	if (plist == NULL || plist->properties == D_SHARED)
		return;

	if (cflag == ENTRY_CHECKPOINT) {
		if (plist->properties == D_THEAD_PRIVATE ||
		    plist->properties == D_PRIVATE ||
		    plist->properties == D_LAST_PRIVATE)
			return;
	} else {
		if (plist->properties == D_THEAD_PRIVATE ||
		    plist->properties == D_PRIVATE ||
		    plist->properties == D_COPY_IN ||
		    plist->properties == D_FIRST_PRIVATE)
			return;
	}

	memset(&item, 0, sizeof(item));
	item.addr = addr;
	memcpy(item.data, word, CAPE_WORD);
	add_item_to_list_ckpt(&item);
}

static void collect_l_words_from_page(unsigned long page_addr,
				      const unsigned char *before_page,
				      const unsigned char *after_page,
				      unsigned char cflag)
{
	size_t off;

	if (data_list_head == NULL)
		return;
	for (off = 0; off < BMP_PAGE_SIZE; off += CAPE_WORD) {
		if (memcmp(before_page + off, after_page + off, CAPE_WORD) != 0)
			collect_l_word(page_addr + off, after_page + off, cflag);
	}
}

unsigned long old_brk = 0, new_brk = 0, heap_top, child_data_start;

struct user_regs_struct save_regs;
unsigned long node;
int num_nodes;
int total_ckpt_flag = 0;
int ckpt_flag = 0; // to save the state of checkpoint that is received

int number_of_packages; // number of data packages is sent at each slave
int current_node=1; //current slave is communicating with master 
int current_job =0; //count the current job
unsigned long number_of_jobs; //save number of step that will be sent from CAPE program
int jobs_per_node; //save the number of step that is divided to a node

unsigned long timespan = 1 ; // timespan of checkpoints

char *pre_node_ip, *next_node_ip, *current_node_ip, * main_node_ip;

//checkpoint variables
unsigned char *after_ckpt, *final_ckpt, *total_ckpt, *buffer_ckpt;
FILE *after_ckpt_stream;
FILE *final_ckpt_stream;
FILE *total_ckpt_stream;
size_t after_ckpt_size, final_ckpt_size, total_ckpt_size;
int buffer_size;

//Workshare checkpoints
unsigned char *mbefore_ckpt;
FILE *mbefore_ckpt_stream;
size_t mbefore_ckpt_size=0;

int task_ckpt_size=0; //size of a workshare checkpoint

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
	unsigned long ucx_sendrecv_calls;
	unsigned long ucx_send_calls, ucx_recv_calls;
	unsigned long ucx_send_bytes, ucx_recv_bytes;
	unsigned long ucx_bootstrap_wait_iters;
	unsigned long sigtrap_count;
	unsigned long generate_ckpt_calls;
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
	unsigned long long ucx_send_ns, ucx_recv_ns;
	unsigned long long ucx_wait_ns;
	unsigned long long ucx_bootstrap_wait_ns;
	unsigned long long generate_ckpt_ns;
} cape_profile_t;

static cape_profile_t cape_profile;

#define CAPE_PROFILE_NS_VAR(name) struct timespec name
#define CAPE_PROFILE_NS_START(name) clock_gettime(CLOCK_MONOTONIC, &(name))
#define CAPE_PROFILE_INC(field) ((void)(cape_profile.field++))
#define CAPE_PROFILE_ADD(field, value) ((void)(cape_profile.field += (value)))
#define CAPE_PROFILE_ADD_NS(field, start) do { \
	struct timespec _now_ts; \
	clock_gettime(CLOCK_MONOTONIC, &_now_ts); \
	cape_profile.field += (unsigned long long)( \
		(_now_ts.tv_sec - (start).tv_sec) * 1000000000ULL \
		+ (_now_ts.tv_nsec - (start).tv_nsec)); \
} while (0)

static void cape_profile_report(void)
{
	fprintf(stderr, "\n[DICKPT PROFILE] Node %ld  (ms = ns/1e6)\n", node);
#define P_NS(name) fprintf(stderr, "  %-30s : %10.3f ms\n", #name, cape_profile.name / 1e6)
#define P_CNT(name) fprintf(stderr, "  %-30s : %lu\n", #name, cape_profile.name)
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
	P_NS(ucx_sendrecv_ns); P_CNT(ucx_sendrecv_calls);
	P_NS(ucx_send_ns); P_CNT(ucx_send_calls); P_CNT(ucx_send_bytes);
	P_NS(ucx_recv_ns); P_CNT(ucx_recv_calls); P_CNT(ucx_recv_bytes);
	P_NS(ucx_wait_ns);
	P_NS(ucx_bootstrap_wait_ns); P_CNT(ucx_bootstrap_wait_iters);
	P_CNT(sigtrap_count);
	fflush(stderr);
#undef P_NS
#undef P_CNT
}
#else
#define CAPE_PROFILE_NS_VAR(name)
#define CAPE_PROFILE_NS_START(name) do { } while (0)
#define CAPE_PROFILE_INC(field) do { } while (0)
#define CAPE_PROFILE_ADD(field, value) do { } while (0)
#define CAPE_PROFILE_ADD_NS(field, start) do { } while (0)
static inline void cape_profile_report(void) {}
#endif

static FILE *open_binary_memstream(unsigned char **bufloc, size_t *sizeloc)
{
	return open_memstream((char **)bufloc, sizeloc);
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

static int cape_userfault_register_range(unsigned long start, unsigned long len)
{
	struct uffdio_register reg;

	if (userfault_fd < 0)
		return 0;

	memset(&reg, 0, sizeof(reg));
	reg.range.start = start;
	reg.range.len = len;
	reg.mode = UFFDIO_REGISTER_MODE_WP;
	if (ioctl(userfault_fd, UFFDIO_REGISTER, &reg) == -1)
		return -1;
	return 0;
}

static int cape_capture_dirty_page(unsigned long fault_addr)
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

	memcpy(&(current_node->data), (const void *)aligned_addr, PAGE_SIZE);

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
	if (cape_capture_dirty_page(page_addr) != 0) {
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
	int rc = 0;

	if (userfault_fd < 0 || !tracking_is_enabled)
		return 0;

	/* Serialize against the uffd worker thread which also reads events. */
	pthread_mutex_lock(&bitmap_lock);
	while (epoll_wait(epoll_fd, &ev, 1, 0) > 0) {
		if ((ev.events & EPOLLIN) == 0)
			break;
		if (cape_handle_userfault_event() != 0) {
			rc = -1;
			break;
		}
	}
	pthread_mutex_unlock(&bitmap_lock);
	return rc;
}

static void *uffd_thread_main(void *unused)
{
	(void)unused;
	while (uffd_thread_run) {
		struct epoll_event ev;
		int n = epoll_wait(epoll_fd, &ev, 1, 100);
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (n == 0) continue;
		if ((ev.events & EPOLLIN) == 0) continue;
		pthread_mutex_lock(&bitmap_lock);
		cape_handle_userfault_event();
		pthread_mutex_unlock(&bitmap_lock);
	}
	return NULL;
}

/* Open a userfaultfd in this process, register all tracked_ranges for
 * write-protect tracking, and start the worker thread that drains
 * events into the dirty bitmap. */
int cape_setup_uffd_self(void)
{
	struct uffdio_api api;
	struct epoll_event ev;
	size_t i;

	if (userfault_fd >= 0)
		return 0;

	userfault_fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (userfault_fd < 0) {
		perror("userfaultfd");
		return 1;
	}

	memset(&api, 0, sizeof(api));
	api.api = UFFD_API;
	api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
	if (ioctl(userfault_fd, UFFDIO_API, &api) == -1) {
		perror("ioctl(UFFDIO_API)");
		return 1;
	}

	for (i = 0; i < tracked_range_count; ++i) {
		if (cape_userfault_register_range(tracked_ranges[i].start,
						  tracked_ranges[i].len) == -1) {
			perror("ioctl(UFFDIO_REGISTER)");
			return 1;
		}
	}

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd < 0) {
		perror("epoll_create1");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = userfault_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, userfault_fd, &ev) < 0) {
		perror("epoll_ctl(userfault_fd)");
		return 1;
	}

	uffd_thread_run = 1;
	if (pthread_create(&uffd_thread, NULL, uffd_thread_main, NULL) != 0) {
		perror("pthread_create(uffd_thread)");
		uffd_thread_run = 0;
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

/* Busy-spin ucp_worker_progress until the request completes. UCX is
 * designed for this; calling ucp_worker_wait between progress calls
 * adds per-iteration epoll latency that destroys bandwidth. */
static void cape_ucx_wait(void *req, size_t expect_len, int check_len,
                          ucp_tag_t *out_tag)
{
	CAPE_PROFILE_NS_VAR(wait_start_ns);
	CAPE_PROFILE_NS_START(wait_start_ns);

    if (out_tag) *out_tag = 0;
    if (req == NULL)
        return;
    if (UCS_PTR_IS_ERR(req)) {
        fprintf(stderr, "CAPE UCX error: %s\n",
                ucs_status_string(UCS_PTR_STATUS(req)));
        exit(1);
    }
    cape_ucx_req_t *r = (cape_ucx_req_t *)req;
    while (!r->completed) {
        ucp_worker_progress(ucp_worker);
        CAPE_PROFILE_INC(ucx_progress_calls);
    }
    CAPE_PROFILE_ADD_NS(ucx_wait_ns, wait_start_ns);
    CAPE_PROFILE_ADD_NS(ucx_progress_ns, wait_start_ns);
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
}

#define CAPE_UCX_TAG(sender_rank, token) \
    ((uint64_t)((((uint32_t)(sender_rank) & 0x0fffU) << 20) | ((uint32_t)(token) & 0x000fffffU)))
#define CAPE_UCX_TAG_MASK  ((uint64_t)0x00000000ffffffffULL)

/* Simultaneous tag send/receive (mimics MPI_Sendrecv). */
static void cape_ucx_sendrecv(
        const void *sendbuf, size_t sendlen, int dest,
        void       *recvbuf, size_t recvlen, int src,
        uint32_t    token)
{
    CAPE_PROFILE_NS_VAR(start_ns);
    CAPE_PROFILE_NS_START(start_ns);
    ucp_tag_t send_tag = CAPE_UCX_TAG(node, token);
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src,  token);

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

    void *rreq = ucp_tag_recv_nbx(ucp_worker, recvbuf, recvlen,
                                   recv_tag, CAPE_UCX_TAG_MASK, &rp);
    void *sreq = ucp_tag_send_nbx(ucp_endpoints[dest], sendbuf, sendlen,
                                   send_tag, &sp);

    cape_ucx_wait(rreq, recvlen, 1, NULL);
    cape_ucx_wait(sreq, 0, 0, NULL);
    CAPE_PROFILE_ADD_NS(ucx_sendrecv_ns, start_ns);
    CAPE_PROFILE_INC(ucx_sendrecv_calls);
    CAPE_PROFILE_ADD(ucx_send_bytes, (uint64_t)sendlen);
    CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)recvlen);
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
    void *req = ucp_tag_send_nbx(ucp_endpoints[dest], buf, len, tag, &sp);
    cape_ucx_wait(req, 0, 0, NULL);
    CAPE_PROFILE_ADD_NS(ucx_send_ns, start_ns);
    CAPE_PROFILE_INC(ucx_send_calls);
    CAPE_PROFILE_ADD(ucx_send_bytes, (uint64_t)len);
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
    void *req = ucp_tag_recv_nbx(ucp_worker, buf, len, tag, CAPE_UCX_TAG_MASK, &rp);
    cape_ucx_wait(req, len, 1, NULL);
    CAPE_PROFILE_ADD_NS(ucx_recv_ns, start_ns);
    CAPE_PROFILE_INC(ucx_recv_calls);
    CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)len);
}

/* === Probed send/recv (single-message, no separate size header) ============
 * Mirrors cape_incr_bitmap.c: sender does one cape_ucx_send; receiver probes
 * the tag to discover the payload size, then issues a single recv sized to
 * the probe result. Removes every "send sz, then send data" pair below. */
#define CAPE_UCX_TAG_TOKEN_MASK  ((uint64_t)0x00000000000fffffULL)

static ucp_tag_message_h cape_ucx_probe_msg_exact(ucp_tag_t recv_tag,
                                                  ucp_tag_recv_info_t *info)
{
    ucp_tag_message_h msg;
    for (;;) {
        msg = ucp_tag_probe_nb(ucp_worker, recv_tag, CAPE_UCX_TAG_MASK, 1, info);
        if (msg != NULL)
            return msg;
        ucp_worker_progress(ucp_worker);
    }
}

static unsigned char *cape_ucx_recv_probe_alloc(size_t *recvlen, int src,
                                                uint32_t token)
{
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src, token);
    ucp_tag_recv_info_t info;
    ucp_tag_message_h msg;
    unsigned char *buf;
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };
    msg = cape_ucx_probe_msg_exact(recv_tag, &info);
    buf = malloc(info.length == 0 ? 1 : info.length);
    if (buf == NULL) { perror("malloc(probed recv)"); exit(1); }
    void *req = ucp_tag_msg_recv_nbx(ucp_worker, buf, info.length, msg, &rp);
    cape_ucx_wait(req, info.length, 1, NULL);
    *recvlen = info.length;
    CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)info.length);
    CAPE_PROFILE_INC(ucx_recv_calls);
    return buf;
}

/* sendrecv with size discovered by probing. Caller supplies a recv buffer
 * with at least `recvcap` bytes; returns the actual bytes received. */
static size_t cape_ucx_sendrecv_probe(
        const void *sendbuf, size_t sendlen, int dest,
        void       *recvbuf, size_t recvcap, int src,
        uint32_t    token)
{
    ucp_tag_t send_tag = CAPE_UCX_TAG(node, token);
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src,  token);
    ucp_tag_recv_info_t info;
    ucp_tag_message_h msg;
    ucp_request_param_t sp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = cape_send_cb
    };
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };

    void *sreq = ucp_tag_send_nbx(ucp_endpoints[dest], sendbuf, sendlen,
                                  send_tag, &sp);
    if (UCS_PTR_IS_ERR(sreq)) {
        fprintf(stderr, "CAPE UCX send failed before probe: %s\n",
                ucs_status_string(UCS_PTR_STATUS(sreq)));
        exit(1);
    }
    msg = cape_ucx_probe_msg_exact(recv_tag, &info);
    if (info.length > recvcap) {
        fprintf(stderr,
                "CAPE UCX: probed message too large (len=%zu cap=%zu)\n",
                info.length, recvcap);
        exit(1);
    }
    void *rreq = ucp_tag_msg_recv_nbx(ucp_worker, recvbuf, info.length, msg, &rp);
    cape_ucx_wait(rreq, info.length, 1, NULL);
    cape_ucx_wait(sreq, 0, 0, NULL);
    CAPE_PROFILE_ADD(ucx_send_bytes, (uint64_t)sendlen);
    CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)info.length);
    CAPE_PROFILE_INC(ucx_sendrecv_calls);
    return info.length;
}

/* Variant that mallocs the recv buffer to the probed size. Used by ring/
 * hypercube allreduce where the peer's payload length varies between
 * iterations. Caller frees the returned buffer. */
static unsigned char *cape_ucx_sendrecv_probe_alloc(
        const void *sendbuf, size_t sendlen, int dest,
        size_t *recvlen, int src,
        uint32_t token)
{
    ucp_tag_t send_tag = CAPE_UCX_TAG(node, token);
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src,  token);
    ucp_tag_recv_info_t info;
    ucp_tag_message_h msg;
    unsigned char *buf;
    ucp_request_param_t sp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = cape_send_cb
    };
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };

    void *sreq = ucp_tag_send_nbx(ucp_endpoints[dest], sendbuf, sendlen,
                                  send_tag, &sp);
    if (UCS_PTR_IS_ERR(sreq)) {
        fprintf(stderr, "CAPE UCX send failed before probe: %s\n",
                ucs_status_string(UCS_PTR_STATUS(sreq)));
        exit(1);
    }
    msg = cape_ucx_probe_msg_exact(recv_tag, &info);
    buf = malloc(info.length == 0 ? 1 : info.length);
    if (buf == NULL) { perror("malloc(probed sendrecv)"); exit(1); }
    void *rreq = ucp_tag_msg_recv_nbx(ucp_worker, buf, info.length, msg, &rp);
    cape_ucx_wait(rreq, info.length, 1, NULL);
    cape_ucx_wait(sreq, 0, 0, NULL);
    CAPE_PROFILE_ADD(ucx_send_bytes, (uint64_t)sendlen);
    CAPE_PROFILE_ADD(ucx_recv_bytes, (uint64_t)info.length);
    CAPE_PROFILE_INC(ucx_sendrecv_calls);
    *recvlen = info.length;
    return buf;
}

/* Tag tokens for different message types to avoid collisions */
#define TAG_CKPT_DATA     0x01
#define TAG_CKPT_FLAG     0x02
#define TAG_BCAST_SIZE    0x03
#define TAG_BCAST_DATA    0x04
#define TAG_SCATTER_SIZE  0x05
#define TAG_SCATTER_DATA  0x06
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
    setenv("UCX_RNDV_THRESH", "inf", 0);

    /* 1. Get rank and size */
#ifdef USE_PMIX
    PMIX_PROC_CONSTRUCT(&pmix_myproc);
    pmix_status_t pst = PMIx_Init(&pmix_myproc, NULL, 0);
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Init failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    node = (unsigned long)pmix_myproc.rank;

    pmix_proc_t wildcard;
    PMIX_PROC_CONSTRUCT(&wildcard);
    PMIX_LOAD_NSPACE(wildcard.nspace, pmix_myproc.nspace);
    wildcard.rank = PMIX_RANK_WILDCARD;

    pmix_value_t *val;
    pst = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val);
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Get(PMIX_JOB_SIZE) failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    num_nodes = (int)val->data.uint32;
    PMIX_VALUE_RELEASE(val);
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
#endif

    /* 2. Initialise UCX context */
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

    /* 3. Create a single-threaded UCX worker */
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

    /* 4. Exchange worker addresses and build endpoints */
    ucp_address_t *local_addr;
    size_t         local_addr_len;
    ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_len);

#ifdef USE_PMIX
    char pmix_key[64];
    snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%ld", node);

    pmix_value_t kval;
    PMIX_VALUE_CONSTRUCT(&kval);
    kval.type          = PMIX_BYTE_OBJECT;
    kval.data.bo.bytes = (char *)local_addr;
    kval.data.bo.size  = local_addr_len;

    pst = PMIx_Put(PMIX_GLOBAL, pmix_key, &kval);
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Put failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    pst = PMIx_Commit();
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Commit failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }
    pmix_info_t fence_info;
    PMIX_INFO_CONSTRUCT(&fence_info);
    PMIX_INFO_LOAD(&fence_info, PMIX_COLLECT_DATA, NULL, PMIX_BOOL);
    pst = PMIx_Fence(NULL, 0, &fence_info, 1);
    PMIX_INFO_DESTRUCT(&fence_info);
    if (pst != PMIX_SUCCESS) {
        fprintf(stderr, "CAPE UCX: PMIx_Fence failed: %s\n",
                PMIx_Error_string(pst));
        exit(1);
    }

    ucp_endpoints = malloc(num_nodes * sizeof(ucp_ep_h));
    for (int i = 0; i < num_nodes; i++) {
        pmix_proc_t peer;
        PMIX_PROC_CONSTRUCT(&peer);
        PMIX_LOAD_NSPACE(peer.nspace, pmix_myproc.nspace);
        peer.rank = (pmix_rank_t)i;

        snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%d", i);
        pmix_value_t *peer_val;
        pst = PMIx_Get(&peer, pmix_key, NULL, 0, &peer_val);
        if (pst != PMIX_SUCCESS) {
            fprintf(stderr, "CAPE UCX: PMIx_Get(addr, rank=%d) failed: %s\n",
                    i, PMIx_Error_string(pst));
            exit(1);
        }
        ucp_ep_params_t ep_params;
        memset(&ep_params, 0, sizeof(ep_params));
        ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
        ep_params.address    = (ucp_address_t *)peer_val->data.bo.bytes;
        st = ucp_ep_create(ucp_worker, &ep_params, &ucp_endpoints[i]);
        PMIX_VALUE_RELEASE(peer_val);
        if (st != UCS_OK) {
            fprintf(stderr, "CAPE UCX: ucp_ep_create(%d) failed: %s\n",
                    i, ucs_status_string(st));
            exit(1);
        }
    }
#else
    const char *jobid    = cape_ucx_bootstrap_id();
    const char *sharedir = cape_ucx_bootstrap_dir();
    ucx_exchange_addresses_via_fs(sharedir, jobid, local_addr, local_addr_len);
#endif

    ucp_worker_release_address(ucp_worker, local_addr);
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
int init_jobs_per_node();
int require_generate_checkpoint();
int require_send_checkpoint();
int require_receive_checkpoint();
int require_inject_checkpoint();
int require_waitfor_checkpoint();
int require_broadcast_checkpoint();
int require_allreduce_checkpoint();
void print_data_in_list(struct shared_data *list);
int cape_setup_uffd_self(void);
int cape_drain_userfaultfd(void);

/* ==========================================================================
 * Library entry points
 * 	cape_init / cape_finalize : open/close UCX + userfaultfd
 * 	cape_register_region      : add a tracked range; if checkpoint tracking is
 * 	                            already active, register + write-protect it now
 * 	cape_start_ckpt           : enable WP tracking on all registered ranges
 * 	cape_stop_ckpt            : disable tracking
 * In the userfaultfd backend the dirty bitmap is filled by a worker thread
 * that drains WP faults from the userfaultfd. The application calls the
 * require_*_checkpoint() functions directly instead of trapping via int3.
 */

void cape_register_region(void *addr, size_t len)
{
	size_t i = tracked_range_count;
	tracked_ranges = realloc(tracked_ranges, (i + 1) * sizeof(*tracked_ranges));
	if (tracked_ranges == NULL) {
		perror("realloc(tracked_ranges)");
		exit(1);
	}
	tracked_ranges[i].start = (unsigned long)addr;
	tracked_ranges[i].len = len;
	tracked_range_count = i + 1;

	if (cape_userfault_register_range(tracked_ranges[i].start,
					  tracked_ranges[i].len) == -1) {
		perror("ioctl(UFFDIO_REGISTER)");
		exit(1);
	}
	if (tracking_is_enabled &&
	    cape_userfault_writeprotect(tracked_ranges[i].start,
					tracked_ranges[i].len, 1) == -1) {
		perror("ioctl(UFFDIO_WRITEPROTECT range)");
		exit(1);
	}
}

/* Append a D_SHARED entry to data_list_head so generate_checkpoint's
 * is_address_shared() mask keeps words in [addr, addr+len). */
static void cape_mark_shared(void *addr, size_t len, unsigned char dtype)
{
	struct shared_data *sd = malloc(sizeof(*sd));
	if (sd == NULL) { perror("malloc(shared_data)"); exit(1); }
	memset(sd, 0, sizeof(*sd));
	sd->addr = (unsigned long)addr;
	sd->len = (unsigned int)len;
	sd->datatype = dtype;
	sd->properties = D_SHARED;
	sd->prev = data_list_tail;
	sd->next = NULL;
	if (data_list_tail) data_list_tail->next = sd; else data_list_head = sd;
	data_list_tail = sd;
}

/* Stable cross-rank allocation for OpenMP-task closure records.
 *
 * A task's captured `shared` variables normally live on the parent's
 * stack, whose VA is only valid on a rank sitting at the same stack
 * frame. The transpiler hoists the captures into a "task environment"
 * struct allocated here.
 *
 * We rely on CAPE's symmetric-memory assumption: ASLR is disabled and
 * every rank runs the identical SPMD allocation sequence, so a plain
 * page-aligned heap allocation returns the *same virtual address* on
 * every rank — no fixed high-address mapping needed. The region is then
 * tracked + marked shared so its words are checkpointed and merged like
 * any other shared region, and inject lands at the matching VA. */
void *cape_task_env_alloc(size_t len)
{
	size_t aligned = (len + (BMP_PAGE_SIZE - 1)) & ~(size_t)(BMP_PAGE_SIZE - 1);
	void *addr;

	if (aligned == 0)
		aligned = BMP_PAGE_SIZE;

	/* Page-aligned because userfaultfd registration needs page
	 * granularity; symmetric layout makes the VA identical across ranks. */
	addr = aligned_alloc(BMP_PAGE_SIZE, aligned);
	if (addr == NULL) { perror("aligned_alloc(task_env)"); exit(1); }
	memset(addr, 0, aligned);

	cape_register_region(addr, aligned);
	cape_mark_shared(addr, aligned, CAPE_UNSIGNED_LONG);
	return addr;
}

int cape_start_ckpt(void)
{
	if (cape_setup_uffd_self() != 0)
		return 1;
	tracking_is_enabled = 1;
	return cape_writeprotect_tracked_ranges(1);
}

int cape_stop_ckpt(void)
{
	int rc = cape_writeprotect_tracked_ranges(0);
	tracking_is_enabled = 0;
	return rc;
}

void cape_init(void)
{
	cape_ucx_init();
}

void cape_finalize(void)
{
	if (uffd_thread_run) {
		uffd_thread_run = 0;
		pthread_join(uffd_thread, NULL);
	}
	if (epoll_fd >= 0) { close(epoll_fd); epoll_fd = -1; }
	if (userfault_fd >= 0) { close(userfault_fd); userfault_fd = -1; }
	free(tracked_ranges);
	tracked_ranges = NULL;
	tracked_range_count = 0;
	cape_ucx_finalize();
	cape_profile_report();
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
/* ----------------------------------------------------------------
 * clear_list_ckpt(): clear list of final_list_ckpt
 * ----------------------------------------------------------------
 */
int clear_list_data_ckpt(struct shared_data_ckpt *list){
	
	if(list == NULL) return 0;	
	struct shared_data_ckpt * temp_node, * current_node;	
	current_node = list;
	while(current_node){
		temp_node = current_node;
		current_node = current_node->next;
		free(temp_node);
	}
	list = NULL;
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
		jobs_per_node = (number_of_jobs + num_nodes - 1) / num_nodes;
	}
	current_job = 0;
	return rc;
}

/* In-process memory access helpers: with no separate monitor process,
 * src/dst are just pointers in our own address space. */
static inline int ioctl_read_data(unsigned int pid, unsigned long src, void *dst, int len){
	(void)pid;
	memcpy(dst, (const void *)src, (size_t)len);
	return 0;
}
static inline int ioctl_write_data(unsigned int pid, const void *src, unsigned long dst, int len){
	(void)pid;
	memcpy((void *)dst, src, (size_t)len);
	return 0;
}
static inline int ioctl_set_write_protect(unsigned int pid, unsigned long dst){
	(void)pid;
	return cape_userfault_writeprotect(dst & ~(PAGE_SIZE - 1), PAGE_SIZE, 1);
}


/* ==================================================================
 * Print data in list: This function to test the list to ensure that it is correct
 * ==================================================================
 */
 void print_data_in_list(struct shared_data *list){
	 struct shared_data *pp;
	 pp = list;
	 if (pp==NULL) printf("\nNode %ld: In the list: EMPTY", node);
	 while(pp!=NULL){
		 printf( "\nNode %ld - In the list : addr = %lx - len = %d  - properties = %d - level =%d - datatype = %d",
		 node, pp->addr, pp->len, pp->properties, pp->level, pp->datatype);	
		 pp = pp->next;
	 }
 }
 
 /* ==================================================================
 * Print data in ckpt list: This function to test the list to ensure that it is correct
 * ==================================================================
 */
 void print_data_in_ckpt_list(struct shared_data_ckpt *list){
	 
	 struct shared_data_ckpt *ppt;
	 ppt = list;
	 
	 while(ppt!=NULL){
		 float f;
		  memcpy(&f, ppt->data, CAPE_WORD);	
		  printf( "\nNode %ld - In CKPT list : addr = %lx  -  Value = %f", node, ppt->addr, f);	
		  
		 ppt = ppt->next;
	 }
 }
 
 
 /* --------------------------------------------------------------------
  * is_in_share_data_list(address, list):
  * to check an address is existed in share_data_list ?
  * input: 
  *  - address
  *  - a pointer that point to share data list
  * output: true or false
  * --------------------------------------------------------------------
  */
  
struct shared_data * is_in_share_data_list(unsigned long int addr, struct shared_data *list){
	struct shared_data *ps = list;
	while(ps != NULL){
		if (addr >= ps->addr && addr < (ps->addr + ps->len)){
			return ps;
		}
		ps = ps->next;
	}
	return NULL;
}

/* ---------------------------------------------------------------------
 * And an item to list checkpoint of share-data attribute variables
 * Input: struct share_data_ckpt *p
 * Output: a list that is managed by list_ckpt_head and list_ckpt_tail
 * ---------------------------------------------------------------------
 */
int add_item_to_list_ckpt(struct shared_data_ckpt *p){
	struct shared_data_ckpt *pt, *tmp;
	
	
	pt = malloc(sizeof(struct shared_data_ckpt));
	if (pt == NULL)
		return 1;
	pt->addr = p->addr ;
	memcpy(pt->data, p->data, CAPE_WORD);
	pt->prev = NULL;
	pt->next = NULL;
	
	float f= 0.0;
	memcpy(&f, pt->data, CAPE_WORD);	
	printf("\nREQUIRE TO ADD IN CKPT LIST: Node %ld : 0x%lx is ADDED to list ckpt - value = %f", node, pt->addr, f);
	
	if(list_ckpt_head == NULL){
		list_ckpt_head = pt;
		list_ckpt_tail = pt;
		
		float f= 0.0;
		memcpy(&f, list_ckpt_head->data, CAPE_WORD);
		printf("\nNEW IN CKPT LIST: Node %ld : 0x%lx is ADDED to list ckpt - value = %f", node, list_ckpt_head->addr, f);	
		
		
		return 0;
	}
	
	//insert at the end of list
	if( pt->addr > list_ckpt_tail->addr){
		pt->prev = list_ckpt_tail ;
		list_ckpt_tail->next = pt;
		list_ckpt_tail = pt;
		return 0;
	}			
			
	//insert at the begin of list
	if (pt->addr < list_ckpt_head->addr){
			pt->next = list_ckpt_head ;
			list_ckpt_head->prev = pt;
			list_ckpt_head = pt;
			return 0;
	}
	
	//find the possition and insert
	tmp = list_ckpt_head;
	while((tmp->next!=NULL) && (tmp->addr < pt->addr))
				tmp = tmp->next;
	
	if(tmp->addr == pt->addr){
		memcpy(tmp->data, pt->data, CAPE_WORD);
		free(pt);
		
		//float f= 0.0;
		//memcpy(&f, tmp->data, CAPE_WORD);	
		//printf("\nAAAAAAAAAAA IN CKPT LIST: Node %ld : 0x%lx is ADDED to list ckpt - value = %f", node, tmp->addr , f);	
		
		return 0;
	}
	
	//printf("\nIN CKPT LIST: Node %ld : 0x%lx is APPENDED to list ckpt", node, pt->addr);	
	pt->next = tmp;
	pt->prev = tmp->prev;
	tmp->prev->next = pt;
	tmp->prev = pt;	
	
	return 0;	

}  


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

	/* Word-level delta: for each faulted page, diff the live post-image
	 * against the pre-image (old_node->data, snapshotted on fault), and
	 * emit only the changed words that fall inside a declared shared
	 * region. Private words and unchanged words are masked out. */
	struct pack_page {
		uint64_t  addr;
		uint8_t   bmp[BMP_WORD_BMP_BYTES];
		uint32_t  n_changed;
		uint32_t *changed;
	};
	struct pack_page *pp = NULL;
	size_t cap = 0, n = 0, k;

	(void)cflag;

	stream = open_binary_memstream(ckpt_data, ckpt_size);

	/* No tracer: in-library checkpoint, register state is not relevant. */
	memset(&save_regs, 0, sizeof(save_regs));

	_timespan = tsp;
	fwrite(&_timespan, sizeof(unsigned long), 1, stream);
	fwrite(&save_regs, sizeof(struct user_regs_struct), 1, stream);

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

		/* In-process: the live page is just our own memory. */
		memcpy(current_page, (const void *)old_node->addr, BMP_PAGE_SIZE);

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

	/* Re-arm write-protect on the pages we just snapshotted. */
	for (old_node = list; old_node != NULL; old_node = old_node->next)
		ioctl_set_write_protect(child_id, old_node->addr);

	fflush(stream);
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
	return rc;
}




int send_checkpoint(int destination){
	/* Single-message send; receiver discovers the size by probing the tag. */
	cape_ucx_send(final_ckpt, final_ckpt_size, destination, TAG_CKPT_DATA);
	fclose(final_ckpt_stream);
	free(final_ckpt);
	final_ckpt = NULL;
	final_ckpt_size = 0;
	return 0;
}

/*---------------------------------------------------------------------
 * join_checkpoint(): to join part S and part L of a checkpoint
 * input: 
 * 		S - is managed by final_ckpt
 * 		L - is managed by list_ckpt_head
 *  output: C - is managed by final_ckpt
 * 		structures of C: [SSD]----------S--------- [ SD] -------L---------
 */
int join_checkpoint (int file_name, struct shared_data_ckpt * list){
	
	 struct shared_data_ckpt *p1;
	 unsigned long ckpt_struct;
	 ckpt_struct = SD; //default value
	 
	 if(list == NULL) return 1;
	 p1 = list;

	//printf("\nCALL JOIN CHECKPOINT\n");

	switch (file_name){
		case FINAL_CHECKPOINT:
				if (final_ckpt_size == 0){
					final_ckpt_stream = open_binary_memstream(&final_ckpt, &final_ckpt_size);
				}
				else{
					fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				}
				//Write signal of data structure
				fwrite(&ckpt_struct, sizeof(unsigned long), 1, final_ckpt_stream);
				//read all node in list and save into checkpoint file
				while(p1!=NULL){
					fwrite(&p1->addr, sizeof(unsigned long), 1, final_ckpt_stream);
					fwrite(p1->data, CAPE_WORD, 1, final_ckpt_stream);
					p1 = p1->next;
				}
				fflush(final_ckpt_stream);
			break;
			
		case TOTAL_CHECKPOINT:
				fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
				//Write signal of data structure
				fwrite(&ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
				//read all node in list and save into checkpoint file
				while(p1!=NULL){
					fwrite(&p1->addr, sizeof(unsigned long), 1, total_ckpt_stream);
					fwrite(p1->data, CAPE_WORD, 1, total_ckpt_stream);
					p1 = p1->next;
				}
				fflush(total_ckpt_stream);

			break;
	} 
	 return 0;	 
 }


int require_send_checkpoint(){
	
	int rc = 0;
	/* Pure-bitmap format: no L section to join. */
	list_ckpt_head = NULL;
	list_ckpt_tail = NULL;
	
	if (node==0){
		current_job++;
		ckpt_flag = 1;	
		
		rc= send_checkpoint(current_node);	
				
		if ((current_job % jobs_per_node == 0) || ((unsigned long)current_job >= number_of_jobs))
		{					
			ckpt_flag = 0;
		}		
		cape_ucx_send(&ckpt_flag, sizeof(int), current_node, TAG_CKPT_FLAG);
		dprintf("Monitor %ld: send checkpoint and ckpt_flag =%d to %d \n",
				node, ckpt_flag, current_node);				
		if (current_job % jobs_per_node == 0) {
			current_node++;			
		}
		
	}
	else
	{
		rc = send_checkpoint(0); // send to master node
	}
	
	if (rc!=0) printf("Monitor %ld: Error on sending checkpoint \n", node);
	return 0;	
}

/* ----------------------------------------
 * receive_checkpoint(): receive final checkpoint and save into a memory stream file 
 * ----------------------------------------
 */
 FILE * receive_checkpoint(int source, unsigned char **ckpt_data, size_t *ckpt_size) {
	size_t nbytes = 0;
	unsigned char *buffer;
	FILE *stream;

	/* Single-message recv: tag probe discovers the payload size. */
	buffer = cape_ucx_recv_probe_alloc(&nbytes, source, TAG_CKPT_DATA);

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

		/* In-process: read the live page, patch only the changed
		 * words, write back. */
		memcpy(live, (const void *)(unsigned long)addrs[i], BMP_PAGE_SIZE);
		for (w = 0; w < BMP_WORDS_PER_PAGE; ++w) {
			if (wbmp_get(word_bmp, w)) {
				if (k >= n_changed) {
					free(changed); rc = 1; goto out;
				}
				live[w] = changed[k++];
			}
		}
		memcpy((void *)(unsigned long)addrs[i], live, BMP_PAGE_SIZE);
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
	unsigned char *buff;
	unsigned long addr, current_sp_addr, ts;
	unsigned char return_addr[4];
	unsigned long file_pointer = 0;
	int len, rc=0; 
	unsigned long current_ckpt_struct;
	current_ckpt_struct = SSD;  	
	
	/* No tracer: registers are not relevant in the library model. */
	memset(regs, 0, sizeof(*regs));
	current_sp_addr = 0;
	(void)return_addr;
	(void)current_sp_addr;
	  	
  	/*
  	 * 1. Open file: No need, because this file have been openned at the receiving time
  	 * 2. Read regestry
  	 * 3. While file!= NULL: Read checkpoint data -> Write to memory
  	 */
  	
  	//Open checkpoint file: at the receiving checkpoint function
  	//Read the Registry information

  	fseek(stream, 0, SEEK_SET); 
  	fread(&ts, sizeof(unsigned long), 1, stream);
  	file_pointer = sizeof(unsigned long);
  	fseek(stream, file_pointer, SEEK_SET);
  	fread(regs, sizeof(struct user_regs_struct), 1, stream);
  	file_pointer += sizeof(struct user_regs_struct);
  	fseek(stream, file_pointer , SEEK_SET);
  	
	/* Pure-bitmap format: stream is just [timespan][save_regs][BMP_S section].
	 * No SSD/SD/MD/EP tail. Find the BMP_S marker and inject pages. */
	(void)buff; (void)current_ckpt_struct; (void)len;
	while (file_pointer < *file_size) {
		if (fread(&addr, sizeof(unsigned long), 1, stream) != 1) {
			rc = 1;
			break;
		}
		file_pointer += sizeof(unsigned long);
		fseek(stream, file_pointer, SEEK_SET);
		if (addr == BMP_S) {
			rc = inject_bitmap_section_from_stream(stream, *file_size,
							       &file_pointer);
			if (rc != 0)
				goto out_close;
			fseek(stream, file_pointer, SEEK_SET);
			continue;
		}
		/* Unknown marker — stale-format checkpoint. Stop. */
		fprintf(stderr,
			"Monitor %ld: unexpected marker 0x%lx in checkpoint stream\n",
			node, addr);
		rc = 1;
		break;
	}

out_close:
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
  
 /* -----------------------------------------------------------------
  * require_inject_checkpoint(): version 3 => version 4
  * 		inject checkpoint into CAPE program memory
  * 	Slave: Inject checkpoint that saved at after_ckpt_stream
  * 	Master: Inject checkpoint that saved at final_ckpt_stream
  * ----------------------------------------------------------------
  */
  int require_inject_checkpoint() {
  	int rc =0;
  	
 // 	print_data_in_ckpt_list(final_list_ckpt_head);
  	
  	//Join L part into checkpoint
//  	rc = join_checkpoint(TOTAL_CHECKPOINT, final_list_ckpt_head);
// 	final_list_ckpt_head = NULL;
// 	final_list_ckpt_tail = NULL;
  	

  	
  	//Inject checkpoint	
	rc = inject_checkpoint_with_write_access(total_ckpt_stream, &total_ckpt_size, &save_regs); 	
  	 
  	
  	return rc;
  }



 
 /*---------------------------------------------------------------------
  * add_to_final_ckpt_list(): L = L1 + L2
  * => add item in sharing-data variables list into L
  * The result will be managed by final_list_ckpt_head
  * --------------------------------------------------------------------
  */
int add_to_final_ckpt_list(struct shared_data_ckpt *plist, struct shared_data *prop){
	struct shared_data_ckpt *pt, *tmp;
	
	pt = malloc(sizeof(struct shared_data_ckpt));
	if (pt == NULL)
		return 1;
	pt->addr = plist->addr ;
	memcpy(pt->data, plist->data, CAPE_WORD);
	pt->prev = NULL;
	pt->next = NULL;

	
	if(final_list_ckpt_head == NULL){
		final_list_ckpt_head = pt;
		final_list_ckpt_tail = pt;
		return 0;
	}
	//insert at the end of list
	if( pt->addr > final_list_ckpt_tail->addr){
		pt->prev = final_list_ckpt_tail ;
		final_list_ckpt_tail->next = pt;
		final_list_ckpt_tail = pt;
		return 0;
	}
	//insert at the begin of list
	if (pt->addr < final_list_ckpt_head->addr){
		pt->next = final_list_ckpt_head ;
		final_list_ckpt_head->prev = pt;
		final_list_ckpt_head = pt;
		return 0;
	}
	
	//find the possition and insert
	tmp = final_list_ckpt_head;
	while((tmp->next!=NULL) && (tmp->addr < pt->addr))
			tmp = tmp->next;
					
	if (tmp->addr == pt->addr) {
		struct shared_data *r = lookup_reduction(pt->addr);
		if (r != NULL) {
			unsigned int sz = reduction_size(r->datatype);
			if (sz == 0 || sz > CAPE_WORD) sz = CAPE_WORD;
			apply_reduction(r->properties, r->datatype,
					tmp->data, pt->data);
			(void)sz;
		} else {
			memcpy(tmp->data, pt->data, CAPE_WORD);
		}
		free(pt);
		return 0;
	}

	pt->next = tmp;
	pt->prev = tmp->prev;
	tmp->prev->next = pt;
	tmp->prev = pt;
	return 0;
	
 }
 

/*----------------------------------------------------------------------
 * merge_data(): vesion CAPE50 
 * 	Merge s_stream file into Total_CKTP file, from s_position
 * -------------------------------------------------------------------- 
 */ 
 int merge_data(FILE * s_stream, \
				unsigned char * s_data, \
				size_t s_size, 
				size_t s_position, int fflag ) {
	
	size_t file_pointer = 0;
	unsigned long current_ckpt_struct;
	unsigned char *buff = NULL;
 	unsigned long addr;
 	int len;
	
	current_ckpt_struct = SSD; //initial checkpoint struct
	
	file_pointer += s_position;
 	fseek(s_stream, (long)file_pointer, SEEK_SET);
 	
 	while(file_pointer < s_size)
 	{
		buff = NULL;
 		//read address from after checkpoint
  		fread(&addr, sizeof(unsigned long), 1, s_stream);
  		file_pointer += sizeof(unsigned long);
 		fseek(s_stream, (long)file_pointer, SEEK_SET);
  		
  		struct shared_data *pro = NULL;
  		struct shared_data_ckpt *plist = NULL;
  		
  		//if addr is the struct signal, not an adress, then we read address again, and write signal into total_ckpt
  		switch(addr){
			case EP:
				fread(&addr, sizeof(unsigned long), 1, s_stream);
				file_pointer += sizeof(unsigned long);
				fseek(s_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = EP;

				//write signal into total checkpoint
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);

				break;
			case SSD:
				fread(&addr, sizeof(unsigned long), 1, s_stream);
				file_pointer += sizeof(unsigned long);
				fseek(s_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = SSD;

				//write signal into total checkpoint
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);

				break;
			case SD:
				fread(&addr, sizeof(unsigned long), 1, s_stream);
				file_pointer += sizeof(unsigned long);
				fseek(s_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = SD;
				//write signal into total checkpoint

				pro = is_in_share_data_list(addr, data_list_head);
				if ((pro!=NULL) && (pro->properties != D_SHARED))
				{
					if (fflag ==FINAL_CHECKPOINT){
						plist = malloc(sizeof(struct shared_data_ckpt));
						if (plist == NULL)
							return 1;
						plist->addr = addr;
						plist->prev = NULL;
						plist->next = NULL;
					}
					break;
				}
				else{
					fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
				}
				break;

			case MD:
				fread(&addr, sizeof(unsigned long), 1, s_stream);
				file_pointer += sizeof(unsigned long);
				fseek(s_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = MD;
				//write signal into total checkpoint
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
				break;
		}

  		//read data and write to total checkpoint
  		switch(current_ckpt_struct){
			case EP:
				len = PAGE_SIZE;
				//read data from checkpoint
				buff = (unsigned char *) malloc(len);
				if (buff == NULL) {
					free(plist);
					return 1;
				}
				fread(buff, len, 1,s_stream);
				file_pointer +=len;
				fseek(s_stream, file_pointer, SEEK_SET);

				//write into total checkpoint
				fwrite(&addr, sizeof(unsigned long), 1, total_ckpt_stream);
				fwrite(buff, len, 1, total_ckpt_stream);
				break;
			case SSD:
				  //read len from checkpoint
				len = 0;
				fread(&len, sizeof(int), 1, s_stream);
				file_pointer += sizeof(int);
				fseek(s_stream, file_pointer, SEEK_SET);

				 //read data from checkpoint
				buff = (unsigned char *) malloc(len);
				if (buff == NULL) {
					free(plist);
					return 1;
				}
				fread(buff, len, 1,s_stream);
				file_pointer +=len;
				fseek(s_stream, file_pointer, SEEK_SET);

				//write into total checkpoint
				fwrite(&addr, sizeof(unsigned long), 1, total_ckpt_stream);
				fwrite(&len, sizeof(int), 1, total_ckpt_stream);
				fwrite(buff, len, 1, total_ckpt_stream);
				break;
			case SD:
				len = CAPE_WORD;
				//read data from checkpoint
				buff = (unsigned char *) malloc(len);
				if (buff == NULL) {
					free(plist);
					return 1;
				}
				fread(buff, len, 1,s_stream);
				file_pointer +=len;
				fseek(s_stream, file_pointer, SEEK_SET);

				if((pro!=NULL) && (pro->properties != D_SHARED))
				{
					if (fflag ==FINAL_CHECKPOINT){
						memcpy(plist->data, buff, len);
						add_to_final_ckpt_list(plist, pro);
					}
					break;
				}
				else{
					//write into final checkpoint
					fwrite(&addr, sizeof(unsigned long), 1, total_ckpt_stream);
					fwrite(buff, len, 1, total_ckpt_stream);
				}

				break;
			case MD:
				break;
		}
		free(buff);
		free(plist);
 	}
	fflush(total_ckpt_stream);
	return 0;
}
 
 
 
 
 
 /* --------------------------------------------------------------------
  * merge_external_checkpoint(): Verion 4.0 => version 5.0
  * => merge checkpoint that is sent from others nodes
  * 	if (t1 >= t2) 
  * 		{ C <- t1 ; C <- R1 ; C <- S2; C <- S1}
  * 	else
  * 		{ C <- t2 ; C <- R2 ; C <- S1; C <- S2}
  * -------------------------------------------------------------------- 
  */
int merge_external_checkpoint(FILE *src_ckpt_stream, 		\
							  unsigned char *src_ckpt_data, \
							  size_t src_ckpt_size 	)
 {
	FILE *tmp_read_stream, *src_read_stream;
	unsigned char *tmp_ckpt;
	size_t tmp_size;
	struct bmp_section_view tmp_bmp, src_bmp;


 	size_t payload_pos;
 	int rc =0;
 	unsigned long t1, t2;

 	if (src_ckpt_size == 0 ) return 1;

 	//IF total_ckpt = NULL: Write Source Checkpoint to Total checkpoint
 	if(total_ckpt_size==0)
 	{
 		total_ckpt_stream = open_binary_memstream(&total_ckpt, &total_ckpt_size);
 		fwrite(src_ckpt_data, src_ckpt_size, 1, total_ckpt_stream);
 		fflush(total_ckpt_stream);

		return 0;
 	}

 	/* Save total_ckpt buffer and close the write stream */
	fflush(total_ckpt_stream);
	tmp_ckpt = total_ckpt;
	tmp_size = total_ckpt_size;
	fclose(total_ckpt_stream);
	total_ckpt_size = 0;

	/* Open READABLE streams from the buffers.
	 * open_memstream is write-only; fread from it returns 0.
	 * Use fmemopen for reading. */
	tmp_read_stream = fmemopen(tmp_ckpt, tmp_size, "rb");
	src_read_stream = fmemopen(src_ckpt_data, src_ckpt_size, "rb");

 	//1. Read timespan
 	fread(&t1, sizeof(unsigned long), 1, tmp_read_stream);
 	fread(&t2, sizeof(unsigned long), 1, src_read_stream);

	total_ckpt_stream = open_binary_memstream(&total_ckpt, &total_ckpt_size);

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

		//Write merged bitmap S into Total_ckpt, with S1 winning conflicts.
		//Reductions are applied per-word inside merge_bitmap_sections.
		//No L tail anymore.
		rc = merge_bitmap_sections(total_ckpt_stream, &src_bmp, &tmp_bmp);
		if (rc != 0)
			goto done;
	}
 	else
 	{
		//Write t2 and R2 into Total_ckpt
		fwrite(src_ckpt_data, sizeof(unsigned long) + sizeof(struct user_regs_struct), 1, total_ckpt_stream);
		fflush(total_ckpt_stream);

		//Write merged bitmap S into Total_ckpt, with S2 winning conflicts.
		rc = merge_bitmap_sections(total_ckpt_stream, &tmp_bmp, &src_bmp);
		if (rc != 0)
			goto done;
	}

done:
	free_bitmap_section(&tmp_bmp);
	free_bitmap_section(&src_bmp);
 	fclose(tmp_read_stream);
 	fclose(src_read_stream);
	free(tmp_ckpt);

	/* Flush so total_ckpt_size reflects all data written by merge_data().
	 * open_memstream only updates the size on fflush/fclose. */
	fflush(total_ckpt_stream);

 	return rc;
 }
 
 
 /* --------------------------------------------------------------
  * require_waitfor_checkpoint(): NOT USE in version 4
  * 
  * Master waiting for checkpoint all slave nodes
  * 	Receive checkpoint from slave nodes
  * 	Merge into final_ckpt
  * --------------------------------------------------------------  
  */
 int require_waitfor_checkpoint() {
 	int rc = 0, i;
 	for(i=1; i <num_nodes; i++)
 	{
 		after_ckpt_stream = receive_checkpoint(i, &after_ckpt, &after_ckpt_size);
 		
 		rc = merge_external_checkpoint(after_ckpt_stream, after_ckpt, after_ckpt_size);
 		fclose(after_ckpt_stream);
 		after_ckpt_size = 0;
 		
 		 		
// 		print_data_in_list(data_list_head);
// 		print_data_in_ckpt_list(final_list_ckpt_head);
 		
 		
 	}
 	
 //	join_checkpoint(TOTAL_CHECKPOINT, final_list_ckpt_head);
 //	final_list_ckpt_head = NULL;
 //	final_list_ckpt_tail = NULL;
 	
 	dprintf("Monitor %ld: After wait for all checkpoint - final_ckpt_size = %zu\n", node, final_ckpt_size);
 	
 	if (rc!=0) printf("Monitor %ld: Error on require_waitfor_checkpoint\n", node);
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
	if(node==0)
	{
		buffer_size = final_ckpt_size;
		buffer_ckpt = final_ckpt;
		/* Single-message broadcast: slaves discover size by probe. */
		for(i = 1; i < num_nodes; i++){
			cape_ucx_send(buffer_ckpt, buffer_size, i, TAG_BCAST_DATA);
		}
	}
	else
	{
		size_t recv_size = 0;
		buffer_ckpt = cape_ucx_recv_probe_alloc(&recv_size, 0, TAG_BCAST_DATA);
		buffer_size = (int)recv_size;
		after_ckpt_stream = open_binary_memstream(&after_ckpt, &after_ckpt_size);
		fwrite(buffer_ckpt, recv_size, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		free(buffer_ckpt);
	}

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
 * find the number lower than n, but it is nearest number that powerof 2
 * ---------------------------------------------------------------------
 */
unsigned int nearest_power_of_two(unsigned int n){
	
	if (is_power_of_two(n)) return n;
	while(n > 1){
		n--;
		if (is_power_of_two(n)) return n;
	}
	return 0;
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

/*---------------------------------------------------------------------*/
// Bcast total_ckpt from master to all nodes

int prepare_allreduce_checkpoint(){
	int rc = 0;
	int i;
	//Bcast total_ckpt from master node to all nodes

	if (node == 0){
		buffer_size = total_ckpt_size;
		buffer_ckpt = total_ckpt;
		for (i = 1; i < num_nodes; i++){
			cape_ucx_send(buffer_ckpt, buffer_size, i, TAG_BCAST_DATA);
		}
	}else{
		size_t recv_size = 0;
		buffer_ckpt = cape_ucx_recv_probe_alloc(&recv_size, 0, TAG_BCAST_DATA);
		buffer_size = (int)recv_size;
		total_ckpt_stream = open_binary_memstream(&total_ckpt, &total_ckpt_size);
		fwrite(buffer_ckpt, recv_size, 1, total_ckpt_stream);
		fflush(total_ckpt_stream);
		free(buffer_ckpt);
	}
	return rc;
}
/*----------------------------------------------------------------------
 * ring_allreduce(): Allreduce using Ring algorithm
 * 
 * ---------------------------------------------------------------------
 */
int ring_allreduce(){
	int rc = 0;

	int message_size;
	unsigned char * recv_buffer, * send_buffer;

	FILE *ckpt_stream;
	unsigned char *ckpt_data;
	size_t ckpt_size;

	int left;
	int right;
	int i;


	send_buffer = total_ckpt   ;
	message_size = total_ckpt_size ;
	int recv_message_size = 0;

	left = ( node - 1 + num_nodes ) % num_nodes;
	right = (node + 1 ) % num_nodes ;



	for(i = 1 ; i < num_nodes; i++){
		uint32_t token_data = TAG_ALLREDUCE_BASE + i;
		size_t recv_len = 0;

		/* Single-message sendrecv; recv buffer allocated to probed size. */
		recv_buffer = cape_ucx_sendrecv_probe_alloc(
		                  send_buffer, (size_t)message_size, right,
		                  &recv_len, left, token_data);
		recv_message_size = (int)recv_len;

		ckpt_stream = open_binary_memstream(&ckpt_data, &ckpt_size);
		fwrite(recv_buffer, recv_message_size, 1, ckpt_stream);
		fflush(ckpt_stream);

		rc = merge_external_checkpoint(ckpt_stream, ckpt_data, ckpt_size);

		fclose(ckpt_stream);
		/* Free the per-iteration open_memstream buffer; otherwise
		 * the monitor leaks O(N) × msg-size per allreduce call. */
		free(ckpt_data);
		ckpt_data = NULL;
		ckpt_size = 0;

		/* Drop the previous send buffer (only the very first
		 * send_buffer aliases total_ckpt and must NOT be freed
		 * here — that one's lifetime is owned by the caller). */
		if (i > 1)
			free(send_buffer);
		send_buffer = recv_buffer;
		message_size = recv_message_size;

	}
	free(recv_buffer);
/*----------------------------------------------------------------------
 * hypercube_allreduce(): Allreduce using hypercube algorithm
 * 
 * ---------------------------------------------------------------------
 */


    return rc;
}


int hypercube_allreduce(){
	int rc = 0;
	int i;
	int nsteps = 0;
	int partner;
	int send_msg_size=0, recv_msg_size=0;
	unsigned char * send_msg;
	unsigned char * recv_msg;

	FILE *ckpt_stream;
	unsigned char *ckpt_data;
	size_t ckpt_size;

	nsteps = mylog2 (num_nodes);

	send_msg = total_ckpt;
	send_msg_size = total_ckpt_size;

	for(i = 0; i < nsteps; i ++){
		uint32_t token_data = TAG_ALLREDUCE_BASE + i;
		size_t recv_len = 0;

		partner = node ^ (1 << i);

		/* Single-message sendrecv; recv buffer allocated to probed size. */
		recv_msg = cape_ucx_sendrecv_probe_alloc(
		               send_msg, (size_t)send_msg_size, partner,
		               &recv_len, partner, token_data);
		recv_msg_size = (int)recv_len;

		ckpt_stream = open_binary_memstream(&ckpt_data, &ckpt_size);
		fwrite(recv_msg, recv_msg_size, 1, ckpt_stream);
		fflush(ckpt_stream);

		rc = merge_external_checkpoint(ckpt_stream, ckpt_data, ckpt_size);

		fclose(ckpt_stream);
		/* open_memstream buffer + UCX recv buffer must be freed
		 * each iteration; otherwise the monitor leaks O(log N) ×
		 * msg-size per allreduce call. */
		free(ckpt_data);
		ckpt_data = NULL;
		ckpt_size = 0;
		free(recv_msg);
		recv_msg = NULL;


		send_msg = total_ckpt;
		send_msg_size = total_ckpt_size;

	}


    return rc;
}

int require_allreduce_checkpoint(){
	int rc = 0;
	//rc = prepare_allreduce_checkpoint();	

	final_list_ckpt_head = list_ckpt_head;
	final_list_ckpt_tail = list_ckpt_tail;
//	print_data_in_ckpt_list(final_list_ckpt_head);
	
	
	rc=  merge_external_checkpoint(final_ckpt_stream, final_ckpt, final_ckpt_size);
	
	
	
	fclose(final_ckpt_stream);
	/* open_memstream's buffer survives fclose — caller must free it.
	 * Without this the per-phase ckpt buffer accumulates and the
	 * monitor's RSS grows as O(phases × peers × dirty_per_phase),
	 * which OOMs at large node counts. */
	free(final_ckpt);
	final_ckpt = NULL;
	final_ckpt_size = 0;
	
	
	

	
	
		
	if (is_power_of_two(num_nodes) && (total_ckpt_size < LARGE_CHECKPOINT)){
		//Use Hypercube algorithm
	    rc = hypercube_allreduce();
		//rc = ring_allreduce();
	}	
	else {	//Large file checkpoint or number of nodes is not power of 2	
		rc = ring_allreduce();
		 
	}	
	
	/* Flush so total_ckpt / total_ckpt_size are up-to-date
	 * (open_memstream only updates on fflush/fclose). */
	fflush(total_ckpt_stream);

	/* open_memstream is write-only — fread from it returns 0.
	 * Open a readable stream from the buffer for injection. */
	fclose(total_ckpt_stream);
	{
		FILE *inject_stream = fmemopen(total_ckpt, total_ckpt_size, "rb");
		size_t inject_size = total_ckpt_size;
		rc = inject_checkpoint_with_write_access(inject_stream, &inject_size, &save_regs);
		/* inject_checkpoint() fcloses the stream internally; do
		 * NOT fclose(inject_stream) here or we double-close. */
	}
	/* Same as final_ckpt above: open_memstream buffer must be freed
	 * explicitly so it doesn't accumulate across allreduce calls. */
	free(total_ckpt);
	total_ckpt = NULL;
	total_ckpt_size = 0;
	clear_list_data_ckpt(final_list_ckpt_head);
	list_ckpt_head = NULL;
	list_ckpt_tail = NULL;
	final_list_ckpt_head = NULL;
	final_list_ckpt_tail = NULL;

	return rc;
}
unsigned long __pc__ = 0;
unsigned long __time_stamp__ = 0;
/* OpenMP-translator scratch globals used by the transpiled apps (see
 * cape.h). cape_begin sets __left__/__right__ for PARALLEL_FOR/FOR; the
 * generated `for (i = __left__; i < __right__; i++)` consumes them. */
int __left__  = -1;
int __right__ = -1;
int __i__     = 0;

int cape_get_node_num(void)  { return (int)node; }
int cape_get_num_nodes(void) { return num_nodes; }

static int cape_split_left (long first, long second, int n, int nn) {
	long long count = (long long)second - (long long)first;
	return (int)(first + (count * (long long)n) / (long long)nn);
}
static int cape_split_right(long first, long second, int n, int nn) {
	long long count = (long long)second - (long long)first;
	return (int)(first + (count * (long long)(n + 1)) / (long long)nn);
}

void __enter_func(void) {}
void __exit_func(void)  {}

int cape_declare_variable(void *addr, unsigned char dtype,
                          unsigned int n_elements, unsigned char ispointer)
{
	unsigned int sz = 4;
	size_t bytes;

	if (!ispointer) {
		switch (dtype) {
			case 1: case 2:                                sz = 1; break;
			case 5: case 6:                                sz = 2; break;
			case 7: case 8: case 9: case 10: case 11:      sz = 4; break;
			case 12:                                       sz = 8; break;
			default:                                       sz = 4;
		}
	}
	bytes = (size_t)sz * (size_t)n_elements;

	cape_register_region(addr, bytes);     /* userfaultfd page tracking */
	cape_mark_shared(addr, bytes, dtype);  /* keep words in is_address_shared mask */
	return 0;
}

int  ckpt_start(void) { return cape_start_ckpt(); }
void ckpt_stop(void)  { (void)cape_stop_ckpt(); }

/* === OpenMP data-sharing clauses ===
 * In the userfaultfd bitmap backend, shared regions are tracked by page
 * faults once registered (via cape_declare_variable / cape_register_region),
 * and private/scalar variables live on the stack and are never tracked.
 * So most clauses are advisory no-ops; only reductions must be recorded so
 * the per-word merge (lookup_reduction) combines instead of overwriting. */
static void cape_add_reduction(void *addr, unsigned char datatype, unsigned char op)
{
	struct shared_data *sd = malloc(sizeof(*sd));
	if (sd == NULL) { perror("malloc(reduction)"); exit(1); }
	memset(sd, 0, sizeof(*sd));
	sd->addr = (unsigned long)addr;
	sd->datatype = datatype;
	sd->len = reduction_size(datatype);
	sd->properties = op;
	sd->prev = data_list_tail;
	sd->next = NULL;
	if (data_list_tail) data_list_tail->next = sd; else data_list_head = sd;
	data_list_tail = sd;
}

void cape_set_default_none(void)        { }
void cape_set_threadprivate(void *addr) { (void)addr; }
void cape_set_shared(void *addr)        { (void)addr; }
void cape_set_private(void *addr)       { (void)addr; }
void cape_set_firstprivate(void *addr)  { (void)addr; }
void cape_set_lastprivate(void *addr)   { (void)addr; }
void cape_set_copyin(void *addr)        { (void)addr; }
void cape_set_copythread(void *addr)    { (void)addr; }
void cape_set_copyprivate(void *addr)   { (void)addr; }

/* OP is the cape_set_reduction operator keyword (CAPE_SUM/MUL/MAX/MIN);
 * map it to the monitor's D_REDUCTION_* range. Datatype defaults to
 * CAPE_INT (the transpiler does not carry the element type here). */
void cape_set_reduction(void *addr, char op)
{
	/* op is a cape.h operator constant (not visible here to avoid header
	 * clashes): CAPE_SUM=8, CAPE_MUL=9, CAPE_MAX=10, CAPE_MIN=11. */
	unsigned char dop;
	switch (op) {
		case 8:  dop = D_REDUCTION_SUM; break;
		case 9:  dop = D_REDUCTION_MUL; break;
		case 10: dop = D_REDUCTION_MAX; break;
		case 11: dop = D_REDUCTION_MIN; break;
		default: dop = D_REDUCTION_SUM; break;
	}
	cape_add_reduction(addr, CAPE_INT, dop);
}

void cape_begin(unsigned char directive, long first, long second)
{
	if (directive == PARALLEL_FOR || directive == FOR ||
	    directive == FOR_NOWAIT) {
		int nn = num_nodes > 0 ? num_nodes : 1;
		int n  = (int)node;
		__left__  = cape_split_left (first, second, n, nn);
		__right__ = cape_split_right(first, second, n, nn);
	}
	cape_start_ckpt();
}

void cape_end(unsigned char directive, unsigned char ops_flag)
{
	(void)directive; (void)ops_flag;
	__time_stamp__ = __pc__++;
	if (require_generate_checkpoint() != 0) {
		fprintf(stderr, "cape_end: generate_checkpoint failed on node %ld\n", node);
		exit(1);
	}
	cape_stop_ckpt();
	if (require_allreduce_checkpoint() != 0) {
		fprintf(stderr, "cape_end: allreduce_checkpoint failed on node %ld\n", node);
		exit(1);
	}
}
