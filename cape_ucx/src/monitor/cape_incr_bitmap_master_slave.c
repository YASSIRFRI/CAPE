#define _GNU_SOURCE

/*
 *	CAPE Monitor: version 5.0 (UCX)
 *		Using Discontinuous incremental checkpointer.
 *		Using UCX for point-to-point and UCC for broadcast collectives
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
#include <linux/userfaultfd.h>
#include <dirent.h>
#include <sys/personality.h>
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

#include "../include/cape_monitor.h"
#include "../include/cape_dickpt_uffd.h"
#include "../include/cape_signal.h"

#include <ucp/api/ucp.h>
#ifdef USE_UCC_BCAST
#include <ucc/api/ucc.h>
#endif
#ifdef USE_PMIX
#include <pmix.h>
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

int process_state = 0; //to follow the state of process
int child_id;
#if 0 /* unused legacy global */
int parent_id;
#endif
int control_fd = -1;
int userfault_fd = -1;
static int epoll_fd = -1;

struct cape_dickpt_range *tracked_ranges = NULL;
size_t tracked_range_count = 0;
int tracking_is_enabled = 0;

/* ===== bitmap S-section format (word-level) =====
 *   [BMP_S:8] [n_dirty_pages:4]
 *   [page_addr_0:8] ... [page_addr_{n-1}:8]      (sorted ascending)
 *   For each dirty page i in order:
 *     [word_bmp_i: BMP_WORD_BMP_BYTES bytes]      (one bit per CAPE_WORD)
 *     [data_i: popcount(word_bmp_i) * CAPE_WORD bytes]
 *
 * Only words that actually changed between snapshot and live memory are
 * emitted, and only for words that don't fall under L-section semantics
 * (reductions and other sharing-data attributes go to L).
 *
 * Merging two S sections: walk both sorted page lists in order; for
 * pages present in both, OR the word bitmaps and resolve per-word
 * conflicts with "newer wins" (the caller chooses which side is newer
 * via the timestamp comparison in merge_external_checkpoint).
 *
 * Inject: load each dirty page's changed words from the stream and
 * write only those words back into child memory at page_addr + w*CAPE_WORD.
 *
 * The L section that follows continues with the existing SD/MD/EP
 * markers unchanged, so reductions and shared-data still work and
 * arithmetic merge of reductions happens at L-merge time. */
#define BMP_S 6
#define BMP_PAGE_SHIFT 12
#define BMP_PAGE_SIZE (1u << BMP_PAGE_SHIFT)
#define BMP_WORDS_PER_PAGE (BMP_PAGE_SIZE / CAPE_WORD)            /* 1024 */
#define BMP_WORD_BMP_BYTES ((BMP_WORDS_PER_PAGE + 7u) >> 3)        /* 128  */

static inline int bmp_get(const unsigned char *bmp, size_t i) {
	return (bmp[i >> 3] >> (i & 7)) & 1;
}
static inline void bmp_set(unsigned char *bmp, size_t i) {
	bmp[i >> 3] |= (unsigned char)(1u << (i & 7));
}

struct shared_data *is_in_share_data_list(unsigned long int addr,
					  struct shared_data *list);
int add_item_to_list_ckpt(struct shared_data_ckpt *p);

/* Returns 1 iff a word at `addr` should be emitted via the L section
 * (and therefore excluded from the S word bitmap). Mirrors the
 * filtering rules previously buried inside collect_l_word(). */
static int word_goes_to_l(unsigned long addr, unsigned char cflag)
{
	struct shared_data *plist;

	plist = is_in_share_data_list(addr, data_list_head);
	if (plist == NULL || plist->properties == D_SHARED)
		return 0;
	if (cflag == ENTRY_CHECKPOINT) {
		if (plist->properties == D_THEAD_PRIVATE ||
		    plist->properties == D_PRIVATE ||
		    plist->properties == D_LAST_PRIVATE)
			return 0;
	} else {
		if (plist->properties == D_THEAD_PRIVATE ||
		    plist->properties == D_PRIVATE ||
		    plist->properties == D_COPY_IN ||
		    plist->properties == D_FIRST_PRIVATE)
			return 0;
	}
	return 1;
}

static uint32_t bmp_word_popcount(const unsigned char *word_bmp)
{
	uint32_t i, n = 0;
	for (i = 0; i < BMP_WORD_BMP_BYTES; ++i)
		n += __builtin_popcount(word_bmp[i]);
	return n;
}

struct bmp_page_view {
	unsigned long addr;
	uint32_t n_words;
	const unsigned char *word_bmp;  /* BMP_WORD_BMP_BYTES bytes */
	const unsigned char *data;      /* n_words * CAPE_WORD bytes */
};

struct bmp_section_view {
	int present;
	size_t start;
	size_t end;
	uint32_t n_pages;
	struct bmp_page_view *pages;
};

static int bmp_page_cmp(const void *a, const void *b)
{
	const struct bmp_page_view *pa = (const struct bmp_page_view *)a;
	const struct bmp_page_view *pb = (const struct bmp_page_view *)b;

	if (pa->addr < pb->addr)
		return -1;
	if (pa->addr > pb->addr)
		return 1;
	return 0;
}

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
	if (view->n_pages > (1u << 24))
		return 1;

	if (view->n_pages != 0) {
		view->pages = calloc(view->n_pages, sizeof(*view->pages));
		if (view->pages == NULL)
			return 1;
	}

	/* First pass: page addresses */
	for (i = 0; i < view->n_pages; ++i) {
		uint64_t a;

		if (ckpt_read_mem(data, size, &pos, &a, sizeof(a)) != 0)
			return 1;
		view->pages[i].addr = (unsigned long)a;
	}

	/* Second pass: word bitmap + sparse word data per page */
	for (i = 0; i < view->n_pages; ++i) {
		struct bmp_page_view *p = &view->pages[i];
		size_t data_bytes;

		if (pos > size || BMP_WORD_BMP_BYTES > size - pos)
			return 1;
		p->word_bmp = data + pos;
		pos += BMP_WORD_BMP_BYTES;

		p->n_words = bmp_word_popcount(p->word_bmp);
		data_bytes = (size_t)p->n_words * CAPE_WORD;
		if (pos > size || data_bytes > size - pos)
			return 1;
		p->data = data + pos;
		pos += data_bytes;
	}

	if (view->n_pages > 1)
		qsort(view->pages, view->n_pages, sizeof(*view->pages),
		      bmp_page_cmp);
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

/* Build merged word bitmap and packed sparse data for one logical page
 * present in older and/or newer. Newer wins on per-word conflicts. */
static void merge_one_page(const struct bmp_page_view *older,
			   const struct bmp_page_view *newer,
			   unsigned char *out_bmp,
			   unsigned char *out_data,
			   uint32_t *out_nwords)
{
	uint32_t w, nw = 0;
	size_t old_off = 0, new_off = 0, out_off = 0;

	memset(out_bmp, 0, BMP_WORD_BMP_BYTES);
	for (w = 0; w < BMP_WORDS_PER_PAGE; ++w) {
		int ob = (older != NULL) && bmp_get(older->word_bmp, w);
		int nb = (newer != NULL) && bmp_get(newer->word_bmp, w);
		const unsigned char *src = NULL;

		if (nb)
			src = newer->data + new_off;
		else if (ob)
			src = older->data + old_off;
		if (nb)
			new_off += CAPE_WORD;
		if (ob)
			old_off += CAPE_WORD;
		if (src != NULL) {
			bmp_set(out_bmp, w);
			memcpy(out_data + out_off, src, CAPE_WORD);
			out_off += CAPE_WORD;
			nw++;
		}
	}
	*out_nwords = nw;
}

static uint32_t count_page_union(const struct bmp_section_view *a,
				 const struct bmp_section_view *b)
{
	uint32_t ia = 0, ib = 0, count = 0;

	while (ia < a->n_pages || ib < b->n_pages) {
		if (ib >= b->n_pages ||
		    (ia < a->n_pages && a->pages[ia].addr < b->pages[ib].addr))
			ia++;
		else if (ia >= a->n_pages ||
			 b->pages[ib].addr < a->pages[ia].addr)
			ib++;
		else { ia++; ib++; }
		count++;
	}
	return count;
}

static int merge_bitmap_sections(FILE *out,
				 const struct bmp_section_view *older,
				 const struct bmp_section_view *newer)
{
	unsigned long marker = BMP_S;
	uint32_t out_pages, ia, ib;
	unsigned char wbmp[BMP_WORD_BMP_BYTES];
	unsigned char wdata[BMP_PAGE_SIZE];

	if (!older->present && !newer->present)
		return 0;

	out_pages = count_page_union(older, newer);
	if (fwrite(&marker, sizeof(marker), 1, out) != 1 ||
	    fwrite(&out_pages, sizeof(out_pages), 1, out) != 1)
		return 1;

	/* Pass 1: merged page address list */
	ia = ib = 0;
	while (ia < older->n_pages || ib < newer->n_pages) {
		uint64_t a;

		if (ib >= newer->n_pages ||
		    (ia < older->n_pages &&
		     older->pages[ia].addr < newer->pages[ib].addr)) {
			a = older->pages[ia++].addr;
		} else if (ia >= older->n_pages ||
			   newer->pages[ib].addr < older->pages[ia].addr) {
			a = newer->pages[ib++].addr;
		} else {
			a = newer->pages[ib].addr;
			ia++;
			ib++;
		}
		if (fwrite(&a, sizeof(a), 1, out) != 1)
			return 1;
	}

	/* Pass 2: per-page merged word bitmap + sparse data */
	ia = ib = 0;
	while (ia < older->n_pages || ib < newer->n_pages) {
		const struct bmp_page_view *o = NULL, *n = NULL;
		uint32_t nw;

		if (ib >= newer->n_pages ||
		    (ia < older->n_pages &&
		     older->pages[ia].addr < newer->pages[ib].addr)) {
			o = &older->pages[ia++];
		} else if (ia >= older->n_pages ||
			   newer->pages[ib].addr < older->pages[ia].addr) {
			n = &newer->pages[ib++];
		} else {
			o = &older->pages[ia++];
			n = &newer->pages[ib++];
		}
		merge_one_page(o, n, wbmp, wdata, &nw);
		if (fwrite(wbmp, BMP_WORD_BMP_BYTES, 1, out) != 1)
			return 1;
		if (nw != 0 &&
		    fwrite(wdata, (size_t)nw * CAPE_WORD, 1, out) != 1)
			return 1;
	}
	return 0;
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

unsigned long child_data_start;
#if 0 /* unused legacy heap/stack tracking globals */
unsigned long old_brk = 0, new_brk = 0, heap_top;
#endif

struct user_regs_struct save_regs;
unsigned long node;
int num_nodes;
int ckpt_flag = 0; // to save the state of checkpoint that is received
#if 0 /* unused legacy checkpoint-routing state */
int total_ckpt_flag = 0;
int number_of_packages; // number of data packages is sent at each slave
#endif

int current_node=1; //current slave is communicating with master 
int current_job =0; //count the current job
unsigned long number_of_jobs; //save number of step that will be sent from CAPE program
int jobs_per_node; //save the number of step that is divided to a node

unsigned long timespan = 1 ; // timespan of checkpoints

#if 0 /* unused legacy socket/IP state */
char *pre_node_ip, *next_node_ip, *current_node_ip, * main_node_ip;
#endif

//checkpoint variables
unsigned char *after_ckpt, *final_ckpt, *total_ckpt, *buffer_ckpt;
FILE *after_ckpt_stream;
FILE *final_ckpt_stream;
FILE *total_ckpt_stream;
size_t after_ckpt_size, final_ckpt_size, total_ckpt_size;
int buffer_size;

#if 0 /* unused V5 workshare checkpoint state */
//Workshare checkpoints
unsigned char *mbefore_ckpt;
FILE *mbefore_ckpt_stream;
size_t mbefore_ckpt_size=0;

int task_ckpt_size=0; //size of a workshare checkpoint

//receive buffer
unsigned char *before_buffer;
#endif




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

	if (header->count > 0) {
		tracked_ranges = malloc(header->count * sizeof(*tracked_ranges));
		if (tracked_ranges == NULL)
			return 1;

		ranges = (struct cape_dickpt_range *)(payload + sizeof(*header));
		memcpy(tracked_ranges, ranges, header->count * sizeof(*tracked_ranges));
		tracked_range_count = header->count;
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

/* Tag tokens for different message types to avoid collisions */
#define TAG_CKPT_DATA     0x01
#define TAG_CKPT_FLAG     0x02
#define TAG_BCAST_SIZE    0x03
#define TAG_BCAST_DATA    0x04
#define TAG_SCATTER_SIZE  0x05
#define TAG_SCATTER_DATA  0x06
#define TAG_ALLREDUCE_BASE 0x100
#define TAG_UCC_OOB_BASE  0x80000

static void *cape_ms_post_recv(void *buf, size_t len, int src, uint32_t token);
static void cape_ms_req_release(void *req);

#ifdef USE_UCC_BCAST
/* =========================================================================
 * UCC State and Helpers
 *
 * UCC needs an OOB allgather for context/team wire-up. We provide that OOB
 * channel using the UCX endpoints that this monitor already initializes.
 * Once the UCC team exists, checkpoint broadcast itself is handled by UCC.
 * ========================================================================= */
static ucc_lib_h     cape_ucc_lib;
static ucc_context_h cape_ucc_context;
static ucc_team_h    cape_ucc_team;
static int           cape_ucc_ready;

struct cape_ucc_oob_req {
    int            n_reqs;
    size_t         msglen;
    void         **send_reqs;
    void         **recv_reqs;
    ucc_status_t   status;
};

static void *cape_ucx_post_send_nb(const void *buf, size_t len,
                                   int dest, uint32_t token)
{
    ucp_tag_t tag = CAPE_UCX_TAG(node, token);
    ucp_request_param_t sp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = cape_send_cb
    };

    return ucp_tag_send_nbx(ucp_endpoints[dest], buf, len, tag, &sp);
}

/* UCC invokes the OOB allgather multiple times during context+team
 * setup (sometimes with the same msglen, sometimes overlapping). Tags
 * keyed on msglen alone collide across calls and let stale stragglers
 * land in the wrong call's recv (the "got=8 expected=364" log line).
 * Use a per-call sequence number bumped atomically — UCC orders OOB
 * collectives consistently across ranks, so every rank's Nth call
 * shares the same seq, giving each call its own tag space. */
static uint32_t cape_ucc_oob_seq = 0;

static uint32_t cape_ucc_oob_token(uint32_t seq, size_t msglen)
{
    /* 11-bit seq (2048 calls) | 8 low bits of msglen — both informative
     * and well within the 20-bit token field of CAPE_UCX_TAG. */
    return TAG_UCC_OOB_BASE
         | ((seq & 0x7ffu) << 8)
         | ((uint32_t)msglen & 0xffu);
}

/* Synchronous ring allgather. Each (seq, step) pair has its own UCX
 * tag — no two messages from anywhere in the program can ever match
 * the same recv. Each cape_ucx_sendrecv blocks until both the local
 * send AND local recv are complete, so step k+1 starts only after
 * step k is fully drained. This makes any cross-call or cross-step
 * tag aliasing impossible. OOB is only a handful of calls during
 * setup, so the serialization cost is irrelevant. */
static ucc_status_t cape_ucc_oob_allgather(void *sbuf, void *rbuf,
                                           size_t msglen, void *coll_info,
                                           void **req)
{
    unsigned char *dst = (unsigned char *)rbuf;
    uint32_t       seq;
    int            i;

    (void)coll_info;

    /* Atomic post-increment. UCC OOB is collective so every rank's
     * k-th invocation gets the same seq value. */
    seq = __sync_fetch_and_add(&cape_ucc_oob_seq, 1u);

    /* Place our own contribution. */
    if (msglen > 0)
        memcpy(dst + (size_t)node * msglen, sbuf, msglen);

    /* Ring exchange: at step i, send to rank+i and recv from rank-i.
     * cape_ucx_sendrecv is fully blocking — it returns only after
     * both ops complete locally. */
    for (i = 1; i < num_nodes && msglen > 0; ++i) {
        int      dest = (int)((node + (unsigned long)i) % (unsigned long)num_nodes);
        int      src  = (int)((node + (unsigned long)num_nodes - (unsigned long)i)
                              % (unsigned long)num_nodes);
        uint32_t tok  = TAG_UCC_OOB_BASE
                      | ((seq & 0x7ffu) << 8)
                      | ((uint32_t)i & 0xffu);

        cape_ucx_sendrecv(sbuf,                       msglen, dest,
                          dst + (size_t)src * msglen, msglen, src,
                          tok);
    }

    /* Synchronous — no async state to track. The non-NULL sentinel
     * lets UCC distinguish "in progress" vs "no request" semantics. */
    *req = (void *)0x1;
    return UCC_OK;
}

static ucc_status_t cape_ucc_oob_allgather_test(void *req)
{
    (void)req;
    /* allgather() returns only after the operation is complete, so
     * test() always returns OK. */
    return UCC_OK;
}

static ucc_status_t cape_ucc_oob_allgather_free(void *req)
{
    (void)req;
    return UCC_OK;
}

static void cape_ucc_fill_oob(ucc_oob_coll_t *oob)
{
    memset(oob, 0, sizeof(*oob));
    oob->allgather = cape_ucc_oob_allgather;
    oob->req_test  = cape_ucc_oob_allgather_test;
    oob->req_free  = cape_ucc_oob_allgather_free;
    oob->coll_info = NULL;
    oob->n_oob_eps = num_nodes;
    oob->oob_ep    = node;
}

static int cape_ucc_check(ucc_status_t status, const char *what)
{
    if (status == UCC_OK)
        return 0;
    fprintf(stderr, "CAPE UCC: %s failed: %s\n",
            what, ucc_status_string(status));
    return 1;
}

static void cape_ucc_init(void)
{
    ucc_lib_config_h     lib_config;
    ucc_context_config_h ctx_config;
    ucc_lib_params_t     lib_params;
    ucc_context_params_t ctx_params;
    ucc_team_params_t    team_params;
    ucc_status_t         status;

    if (cape_ucc_ready)
        return;

    memset(&lib_params, 0, sizeof(lib_params));
    lib_params.mask        = UCC_LIB_PARAM_FIELD_THREAD_MODE;
    lib_params.thread_mode = UCC_THREAD_SINGLE;

    if (cape_ucc_check(ucc_lib_config_read(NULL, NULL, &lib_config),
                       "ucc_lib_config_read"))
        exit(1);
    if (cape_ucc_check(ucc_init(&lib_params, lib_config, &cape_ucc_lib),
                       "ucc_init")) {
        ucc_lib_config_release(lib_config);
        exit(1);
    }
    ucc_lib_config_release(lib_config);

    memset(&ctx_params, 0, sizeof(ctx_params));
    ctx_params.mask = UCC_CONTEXT_PARAM_FIELD_OOB;
    cape_ucc_fill_oob(&ctx_params.oob);

    if (cape_ucc_check(ucc_context_config_read(cape_ucc_lib, NULL, &ctx_config),
                       "ucc_context_config_read"))
        exit(1);
    if (cape_ucc_check(ucc_context_create(cape_ucc_lib, &ctx_params,
                                          ctx_config, &cape_ucc_context),
                       "ucc_context_create")) {
        ucc_context_config_release(ctx_config);
        exit(1);
    }
    ucc_context_config_release(ctx_config);

    memset(&team_params, 0, sizeof(team_params));
    team_params.mask = UCC_TEAM_PARAM_FIELD_OOB;
    cape_ucc_fill_oob(&team_params.oob);

    if (cape_ucc_check(ucc_team_create_post(&cape_ucc_context, 1,
                                            &team_params, &cape_ucc_team),
                       "ucc_team_create_post"))
        exit(1);
    while ((status = ucc_team_create_test(cape_ucc_team)) == UCC_INPROGRESS) {
        if (cape_ucc_check(ucc_context_progress(cape_ucc_context),
                           "ucc_context_progress(team_create)"))
            exit(1);
    }
    if (cape_ucc_check(status, "ucc_team_create_test"))
        exit(1);

    cape_ucc_ready = 1;
}

static void cape_ucc_finalize(void)
{
    if (!cape_ucc_ready)
        return;
    cape_ucc_check(ucc_team_destroy(cape_ucc_team), "ucc_team_destroy");
    cape_ucc_check(ucc_context_destroy(cape_ucc_context), "ucc_context_destroy");
    cape_ucc_check(ucc_finalize(cape_ucc_lib), "ucc_finalize");
    cape_ucc_team = NULL;
    cape_ucc_context = NULL;
    cape_ucc_lib = NULL;
    cape_ucc_ready = 0;
}

static int cape_ucc_bcast(void *buffer, size_t count,
                          ucc_datatype_t datatype, const char *what)
{
    ucc_coll_args_t args;
    ucc_coll_req_h  req;
    ucc_status_t    status;
    int             rc = 0;

    if (count == 0)
        return 0;

    memset(&args, 0, sizeof(args));
    args.mask              = 0;
    args.coll_type         = UCC_COLL_TYPE_BCAST;
    args.root              = 0;
    args.src.info.buffer   = buffer;
    args.src.info.count    = count;
    args.src.info.datatype = datatype;
    args.src.info.mem_type = UCC_MEMORY_TYPE_HOST;

    status = ucc_collective_init(&args, &req, cape_ucc_team);
    if (cape_ucc_check(status, what))
        return 1;

    status = ucc_collective_post(req);
    if (cape_ucc_check(status, what)) {
        ucc_collective_finalize(req);
        return 1;
    }

    while ((status = ucc_collective_test(req)) == UCC_INPROGRESS) {
        rc = cape_ucc_check(ucc_context_progress(cape_ucc_context), what);
        if (rc != 0)
            break;
    }
    if (rc == 0)
        rc = cape_ucc_check(status, what);

    ucc_collective_finalize(req);
    return rc;
}
#endif

/* =========================================================================
 * Master-slave parallel-receive helpers
 *
 * Master keeps one "slot" per slave so all N-1 slaves can transmit
 * concurrently — UCX progresses every posted recv in parallel inside a
 * single worker thread, so this gives us peak ingress with no extra
 * threads. Each slot is a tiny state machine: post a size header recv,
 * then once the size is known post a data recv, then enqueue for
 * synchronous merge into the master's total checkpoint.
 *
 * Memory is bounded by CAPE_MS_MAX_QUEUE_BYTES: a slot that has already
 * received its size header does not allocate the data buffer or post
 * the data recv until the in-flight payload total fits under the cap.
 * That's the back-pressure knob — increase it for more parallelism,
 * decrease it to spare RAM. Slaves block in their send until the master
 * posts the recv, so this throttle propagates naturally.
 * ========================================================================= */
#ifndef CAPE_MS_MAX_QUEUE_BYTES
#define CAPE_MS_MAX_QUEUE_BYTES (256ull * 1024ull * 1024ull)  /* 256 MB */
#endif

enum {
    CAPE_MS_IDLE = 0,
    CAPE_MS_RECV_SIZE,   /* size_req in flight */
    CAPE_MS_HAVE_SIZE,   /* size received but data recv held back (queue full) */
    CAPE_MS_RECV_DATA,   /* data_req in flight, payload buffer reserved */
    CAPE_MS_DONE         /* fully received, awaiting merge */
};

struct cape_ms_slot {
    int            sender;        /* slave rank */
    int            state;
    int32_t        payload_size;  /* size header */
    void          *size_req;
    void          *data_req;
    unsigned char *payload;
};

static void *cape_ms_post_recv(void *buf, size_t len, int src, uint32_t token)
{
    ucp_tag_t tag = CAPE_UCX_TAG(src, token);
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                      | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };
    void *req = ucp_tag_recv_nbx(ucp_worker, buf, len, tag,
                                  CAPE_UCX_TAG_MASK, &rp);
    if (UCS_PTR_IS_ERR(req)) {
        fprintf(stderr, "CAPE master-slave: tag_recv post failed: %s\n",
                ucs_status_string(UCS_PTR_STATUS(req)));
        exit(1);
    }
    return req;
}

static int cape_ms_req_done(void *req)
{
    if (req == NULL)
        return 1;
    cape_ucx_req_t *r = (cape_ucx_req_t *)req;
    return r->completed;
}

static void cape_ms_req_release(void *req)
{
    if (req == NULL)
        return;
    cape_ucx_req_t *r = (cape_ucx_req_t *)req;
    r->completed  = 0;
    r->status     = UCS_OK;
    r->recv_len   = 0;
    r->sender_tag = 0;
    ucp_request_free(req);
}

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

#ifdef USE_UCC_BCAST
    cape_ucc_init();
#endif
}

/* Finalize UCX (close endpoints, destroy worker, cleanup context) */
static void cape_ucx_finalize(void)
{
#ifdef USE_UCC_BCAST
    cape_ucc_finalize();
#endif

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
int send_int_value_to_child(int value);
int get_long_int_from_child(unsigned long *value);
int init_jobs_per_node();
#if 0 /* unused legacy helpers */
int read_current_stack_start(unsigned int pid, unsigned long src, unsigned long dst, int len);
int read_current_brk(unsigned int pid, unsigned long src, unsigned long dst, int len);
#endif
int ioctl_read_data(unsigned int pid, unsigned long src, void *dst, int len);
int ioctl_write_data(unsigned int pid, const void *src, unsigned long dst, int len);
#if 0 /* unused legacy helper */
int ioctl_clear_write_protect(unsigned int pid, unsigned long dst);
#endif
void tracer_wait ( pid_t pid, int * status, int options, struct user * u );
int lock_process_memory(unsigned int pid);
int unlock_process_memory(unsigned int pid);
int require_generate_checkpoint();
int require_send_checkpoint();
int require_receive_checkpoint();
int require_inject_checkpoint();
int require_waitfor_checkpoint();
int require_broadcast_checkpoint();
int read_shared_data();
void end_shared_data();

/* V5: unused */
// int init_generate_workshare_checkpoint(unsigned int ntask);
// int require_generate_workshare_checkpoint();
// int require_generate_total_checkpoint();
// int require_scatter_checkpoint();
// int require_inject_workshare_checkpoint();

int require_allreduce_checkpoint();
#if 0 /* no implementation in this monitor */
int allreduce_checkpoint();
#endif
#if 0 /* debug-only printer */
void print_data_in_list(struct shared_data *list);
#endif
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

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control_pair) == -1) {
		perror("socketpair");
		return 1;
	}


	//fork a process to run CAPE program
	switch ( child_id = fork ( ) ) {
		case -1 :	/* Error */
			perror ( "fork" ) ;
			return 1 ;
		case 0 :	/* Child */
			close(control_pair[0]);
			snprintf(control_fd_text, sizeof(control_fd_text), "%d", control_pair[1]);
			setenv(CAPE_DICKPT_ENV_SOCK_FD, control_fd_text, 1);
			/* Disable ASLR before exec so the app's globals (bss/data/heap/
			 * mmap) land at identical VAs on every rank. The personality
			 * flag persists across execve, so the app sees a non-randomized
			 * address space without needing to re-exec itself. */
			{
				int _p = personality(0xffffffff);
				if (_p >= 0 && !(_p & ADDR_NO_RANDOMIZE))
					personality((unsigned long)_p | ADDR_NO_RANDOMIZE);
			}
			ptrace ( PTRACE_TRACEME, NULL, NULL, NULL ) ; //This process will be traced
			execv ( exec_file , &argv[1] );
			perror (exec_file) ;
			return 2 ;
		default :	/* Parent */
			control_fd = control_pair[0];
			close(control_pair[1]);
			/* Initialise UCX in the parent AFTER the fork. Doing it before
			 * the fork left UCX's memory-registration cache pointing at
			 * pre-fork physical pages that get broken by COW the first
			 * time we write to a checkpoint buffer, forcing UCX onto a
			 * slow per-send memcpy fallback (~20× slowdown vs cape.c).
			 * Only the parent talks UCX; the child execs the app. */
			cape_ucx_init();
			break;
	}
	
	

	//wait for cape program finish startup	
	for ( process_state = 0 ; process_state != 2 ; ) {
		tracer_wait ( child_id, & status, 0, & u ) ;
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

	ptrace(PTRACE_CONT, child_id, NULL, NULL ) ;

	//Monitor CAPE program
	while(1) {
		if (cape_wait_for_child_event(child_id, &status) == -1) {
			perror("waitpid");
			break;
		}
		if(WIFEXITED(status)) {
			break;
		}
		if(WIFSIGNALED(status)) {
			break;
		}
		if(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
				int dx = 0;
				ptrace(PTRACE_GETREGS, child_id, NULL, &regs);
				dx = regs.rdx;
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
						break;
						
					case S_SEND_CHECKPOINT:	
						rc = require_send_checkpoint();
						if(rc != 0)	exit(1);						
						break;
						
					case S_RECEIVE_CHECKPOINT: 
						rc = require_receive_checkpoint();
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
		jobs_per_node = (number_of_jobs + num_nodes - 1) / num_nodes;
	}
	current_job = 0;
	return rc;	
	
 }
/* ---------------------------------------------------
 * read_current_stack_start(): read the start adress of stack 
 * ---------------------------------------------------
 */
#if 0 /* unused legacy /proc maps helper */
int read_current_stack_start(unsigned int pid, unsigned long src, unsigned long dst, int len){
	char maps_path[64];
	FILE *maps;
	char line[512];
	unsigned long long start, end;
	char perms[5];

	snprintf(maps_path, sizeof(maps_path), "/proc/%u/maps", pid);
	maps = fopen(maps_path, "r");
	if (maps == NULL)
		return 0;

	while (fgets(line, sizeof(line), maps) != NULL) {
		if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3)
			continue;
		if (strstr(line, "[stack]") == NULL)
			continue;
		fclose(maps);
		return (int)start;
	}

	fclose(maps);
	return 0;
}
#endif
/* ---------------------------------------------------
 * read_current_brk(): read the top address of heap
 * ---------------------------------------------------
 */
#if 0 /* unused legacy /proc maps helper */
int read_current_brk(unsigned int pid, unsigned long src, unsigned long dst, int len){
	char maps_path[64];
	FILE *maps;
	char line[512];
	unsigned long long start, end;
	char perms[5];

	snprintf(maps_path, sizeof(maps_path), "/proc/%u/maps", pid);
	maps = fopen(maps_path, "r");
	if (maps == NULL)
		return 0;

	while (fgets(line, sizeof(line), maps) != NULL) {
		if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3)
			continue;
		if (strstr(line, "[heap]") == NULL)
			continue;
		fclose(maps);
		return (int)end;
	}

	fclose(maps);
	return 0;
}
#endif
/* -----------------------------------------------------
 * ioctl_read_data(): Read data from memory of the process
 * -----------------------------------------------------
 */
int ioctl_read_data(unsigned int pid, unsigned long src, void *dst, int len){
	return read_remote_memory(pid, src, dst, (size_t)len) == 0 ? 1 : 0;
}
/* -----------------------------------------------------
 * ioctl_write_data(): wite data into memory of the process
 * -----------------------------------------------------
 */
int ioctl_write_data(unsigned int pid, const void *src, unsigned long dst, int len){
	return write_remote_memory(pid, src, dst, (size_t)len);
}
/* -----------------------------------------------------------------
 * iotcl_clear_write_protect(): Version 2.0
 * Clear write protected and save address, data of a page
 * 	Data wil be save into a list and manage by list_head and list_end
 * 	Data structure: <addr, data>, <addr, data>,...
 * 	This list will be sorted by ascending
 * -----------------------------------------------------------------
 */
#if 0 /* unused with userfaultfd write-protect path */
 int ioctl_clear_write_protect(unsigned int pid, unsigned long dst){
	if (cape_capture_dirty_page(pid, dst) != 0)
		return 1;
	if (cape_userfault_writeprotect(dst & ~(PAGE_SIZE - 1), PAGE_SIZE, 0) == -1)
		return 1;
	return 0;
}
#endif

/* ---------------------------------------------------------------
 * ioctl_set_write_protect(): Version 3.0
 * 	Set write protect to a page that contain dst address
 * ---------------------------------------------------------------
 */
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
	dprintf("Monitor %ld: shared-data metadata stream is not implemented in the userfaultfd backend\n",
		node);
	return 0;
}
/* ---------------------------------------------------------------------
 * end_shared_data: to close the sharing data off current level
 * 
 * ---------------------------------------------------------------------
 */
void end_shared_data(){
	unsigned char level;
	struct shared_data *pp;
	
	if (data_list_tail !=NULL)
		level = data_list_tail ->level;
	
	while((data_list_tail->prev !=NULL) && (data_list_tail->level == level)){
		pp = data_list_tail;
		data_list_tail = data_list_tail ->prev;
		data_list_tail->next = NULL;
		free(pp);
		pp= NULL;		
	}
	if (data_list_tail->level == level){
		free(data_list_tail);
		data_list_head = NULL;
		data_list_tail = NULL;
	}
	
}


/* ==================================================================
 * Print data in list: This function to test the list to ensure that it is correct
 * ==================================================================
 */
#if 0 /* debug-only helper, no live callers */
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
#endif
 
 /* ==================================================================
 * Print data in ckpt list: This function to test the list to ensure that it is correct
 * ==================================================================
 */
#if 0 /* debug-only helper, no live callers */
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
#endif
 
 
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
	uint32_t n_ranges;
	size_t i;
	CAPE_PROFILE_NS_VAR(profile_start_ns);
	CAPE_PROFILE_NS_START(profile_start_ns);
	
	//open the stream memory file
	stream = open_binary_memstream(ckpt_data, ckpt_size);		
		
	//get the registers
	ptrace(PTRACE_GETREGS, child_id, NULL, &save_regs);
	
	//write timespan into checkpoint
	_timespan = tsp;
	fwrite(&_timespan, sizeof(unsigned long), 1, stream);

	//save register to file
	fwrite(&save_regs,sizeof(struct user_regs_struct),1, stream);

	{
	struct dirty_page {
		unsigned long addr;
		uint32_t n_words;
		unsigned char word_bmp[BMP_WORD_BMP_BYTES];
		unsigned char page[BMP_PAGE_SIZE];
	};
	struct dirty_page *emit = NULL;
	size_t cap = 0, count = 0, j;
	uint32_t n_dirty;

	(void)i;
	(void)n_ranges;

	for (old_node = list; old_node != NULL; old_node = old_node->next) {
		unsigned char current_page[BMP_PAGE_SIZE];
		unsigned char word_bmp[BMP_WORD_BMP_BYTES];
		uint32_t nw = 0, w;
		size_t off;

		if (read_remote_memory(child_id, old_node->addr,
				       current_page, BMP_PAGE_SIZE) != 0) {
			fprintf(stderr,
				"Monitor %ld: failed to read dirty page 0x%lx\n",
				node, old_node->addr);
			continue;
		}
		if (memcmp(old_node->data, current_page, BMP_PAGE_SIZE) == 0)
			continue;

		memset(word_bmp, 0, sizeof(word_bmp));
		for (w = 0, off = 0; off < BMP_PAGE_SIZE;
		     off += CAPE_WORD, ++w) {
			unsigned long waddr = old_node->addr + off;
			const unsigned char *before =
				(const unsigned char *)old_node->data + off;
			const unsigned char *after = current_page + off;

			if (memcmp(before, after, CAPE_WORD) == 0)
				continue;
			if (word_goes_to_l(waddr, cflag)) {
				/* Reduction / sharing-data attribute word —
				 * routed through the L section so reduction
				 * arithmetic happens at merge time. */
				collect_l_word(waddr, after, cflag);
				continue;
			}
			bmp_set(word_bmp, w);
			nw++;
		}
		if (nw == 0)
			continue;

		if (count == cap) {
			cap = cap ? cap * 2 : 32;
			emit = realloc(emit, cap * sizeof(*emit));
			if (emit == NULL) {
				perror("realloc(emit)");
				exit(1);
			}
		}
		emit[count].addr = old_node->addr;
		emit[count].n_words = nw;
		memcpy(emit[count].word_bmp, word_bmp, BMP_WORD_BMP_BYTES);
		memcpy(emit[count].page, current_page, BMP_PAGE_SIZE);
		count++;
	}

	/* Sort dirty pages by address so the on-disk format is canonical
	 * (and so merge_bitmap_sections can walk both sides linearly). */
	if (count > 1) {
		/* simple insertion sort — count is small in practice */
		for (j = 1; j < count; ++j) {
			struct dirty_page tmp = emit[j];
			size_t k = j;
			while (k > 0 && emit[k - 1].addr > tmp.addr) {
				emit[k] = emit[k - 1];
				--k;
			}
			emit[k] = tmp;
		}
	}

	n_dirty = (uint32_t)count;
	fwrite(&marker, sizeof(marker), 1, stream);
	fwrite(&n_dirty, sizeof(n_dirty), 1, stream);
	for (j = 0; j < count; ++j) {
		uint64_t a = (uint64_t)emit[j].addr;
		fwrite(&a, sizeof(a), 1, stream);
	}
	for (j = 0; j < count; ++j) {
		uint32_t w;
		fwrite(emit[j].word_bmp, BMP_WORD_BMP_BYTES, 1, stream);
		for (w = 0; w < BMP_WORDS_PER_PAGE; ++w) {
			if (bmp_get(emit[j].word_bmp, w))
				fwrite(emit[j].page + (size_t)w * CAPE_WORD,
				       CAPE_WORD, 1, stream);
		}
	}
	free(emit);
	}

	for (old_node = list; old_node != NULL; old_node = old_node->next) {
		ioctl_set_write_protect(child_id, old_node->addr);
	}

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
	int sz = (int)final_ckpt_size;
	cape_ucx_send(&sz, sizeof(int), destination, TAG_CKPT_DATA);
	cape_ucx_send(final_ckpt, final_ckpt_size, destination, TAG_CKPT_DATA + 1);
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
	switch (file_name){
		case FINAL_CHECKPOINT:
			   //Open checkpoint file if it is closed
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


/* ------------------------------------------------------------------------
 * require_send_checkpoint(): version 3.0 => NOT USED in version 4.0
 * 	Require send checkpoint from to slave, and from slave to master
 * if (Master)
 * 	Send checkpoint and flag_ckpt to slave
 * 	flag_ckpt = 0 if this is the lastest checkpoint, orthewhise flag_ckpt = 1
 * 
 * TODO: implement the scheduling to distribute checkpoint to slave  *  
 * -------------------------------------------------------------------------
 */
int require_send_checkpoint(){
	int rc = 0;
	/* Always join S + L before sending so the on-wire checkpoint is
	 * complete (reductions + sharing-data attribute words live in L). */
	join_checkpoint(FINAL_CHECKPOINT, list_ckpt_head);
	list_ckpt_head = NULL;
	list_ckpt_tail = NULL;

	if (node == 0) {
		/* Master keeps its own final_ckpt in place; no peer-to-peer
		 * forwarding. require_waitfor_checkpoint() folds the master's
		 * own ckpt into the merged total alongside slaves'. */
		return 0;
	}

	/* Slave: ship our checkpoint to the master and we're done. */
	rc = send_checkpoint(0);
	if (rc != 0)
		printf("Monitor %ld: Error on sending checkpoint \n", node);
	return rc;
}

/* ----------------------------------------
 * receive_checkpoint(): receive final checkpoint and save into a memory stream file 
 * ----------------------------------------
 */
 FILE * receive_checkpoint(int source, unsigned char **ckpt_data, size_t *ckpt_size) {
	int nbytes;
	unsigned char *buffer;
	FILE *stream;

	/* Receive the size first */
	cape_ucx_recv(&nbytes, sizeof(int), source, TAG_CKPT_DATA);
	buffer = malloc(nbytes);
	/* Receive the checkpoint data */
	cape_ucx_recv(buffer, nbytes, source, TAG_CKPT_DATA + 1);

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
	uint32_t n_dirty, i;
	unsigned long *addrs = NULL;

	if (fread(&n_dirty, sizeof(n_dirty), 1, stream) != 1)
		return 1;
	*file_pointer += sizeof(n_dirty);

	if (n_dirty == 0)
		return 0;
	if (n_dirty > (1u << 24))
		return 1;

	if (*file_pointer > file_size ||
	    (size_t)n_dirty * sizeof(uint64_t) > file_size - *file_pointer)
		return 1;

	addrs = malloc((size_t)n_dirty * sizeof(*addrs));
	if (addrs == NULL)
		return 1;
	for (i = 0; i < n_dirty; ++i) {
		uint64_t a;

		if (fread(&a, sizeof(a), 1, stream) != 1) {
			free(addrs);
			return 1;
		}
		addrs[i] = (unsigned long)a;
		*file_pointer += sizeof(a);
	}

	for (i = 0; i < n_dirty; ++i) {
		unsigned char word_bmp[BMP_WORD_BMP_BYTES];
		uint32_t w, nw, run_start = 0, in_run = 0;

		if (*file_pointer > file_size ||
		    BMP_WORD_BMP_BYTES > file_size - *file_pointer) {
			free(addrs);
			return 1;
		}
		if (fread(word_bmp, BMP_WORD_BMP_BYTES, 1, stream) != 1) {
			free(addrs);
			return 1;
		}
		*file_pointer += BMP_WORD_BMP_BYTES;

		nw = bmp_word_popcount(word_bmp);
		if (*file_pointer > file_size ||
		    (size_t)nw * CAPE_WORD > file_size - *file_pointer) {
			free(addrs);
			return 1;
		}

		/* Walk the bitmap; coalesce consecutive set bits into runs and
		 * write each run with one ioctl. We only modify words whose bit
		 * is set, leaving the rest of the child's page untouched. */
		for (w = 0; w <= BMP_WORDS_PER_PAGE; ++w) {
			int bit = (w < BMP_WORDS_PER_PAGE) &&
				  bmp_get(word_bmp, w);

			if (bit && !in_run) {
				run_start = w;
				in_run = 1;
			} else if (!bit && in_run) {
				uint32_t run_len = w - run_start;
				size_t bytes = (size_t)run_len * CAPE_WORD;
				unsigned char *buf = malloc(bytes);
				unsigned long dst;
				int rc;

				if (buf == NULL) {
					free(addrs);
					return 1;
				}
				if (fread(buf, bytes, 1, stream) != 1) {
					free(buf);
					free(addrs);
					return 1;
				}
				*file_pointer += bytes;
				dst = addrs[i] +
				      (unsigned long)run_start * CAPE_WORD;
				rc = ioctl_write_data(child_id, buf, dst,
						      (int)bytes);
				free(buf);
				if (rc != 0) {
					fprintf(stderr,
						"Monitor %ld: failed to inject bitmap words at 0x%lx len=%zu: %s\n",
						node, dst, bytes,
						strerror(errno));
					free(addrs);
					return rc;
				}
				in_run = 0;
			}
		}
	}

	free(addrs);
	return 0;
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
	
	//save the next instruction
	ptrace(PTRACE_GETREGS, child_id, NULL, regs);
	current_sp_addr = regs->rsp;
	ioctl_read_data(child_id, current_sp_addr + 4, return_addr, 4);
	  	
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
  	
	//printf("\nNode %ld: prepare to inject checkpoint", node);
  	while(file_pointer < *file_size)
  	{
  		//read address or signal from checkpoint
  		fread(&addr, sizeof(unsigned long), 1, stream);
  		file_pointer += sizeof(unsigned long);
  		fseek(stream, file_pointer, SEEK_SET); 	
  		
  		//printf("\nNode %ld: addr = %ld", node, addr);
  		
  		//it it is signal, read address again
  		switch(addr){
			case BMP_S:
				rc = inject_bitmap_section_from_stream(stream, *file_size,
								       &file_pointer);
				if (rc != 0)
					goto out_close;
				fseek(stream, file_pointer, SEEK_SET);
				continue;
			case SSD:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = SSD;
				break;
			case SD:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = SD;	
				break;
			case EP:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = EP;	
				break;
			case MD:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = MD;
				break;
		}
		
  		
  		//read data from checkpoint  		
  		//read len from checkpoint
  		len = 0;
  		switch(current_ckpt_struct){
			case EP:
				len = PAGE_SIZE;
				break;
			case SSD:
				fread(&len, sizeof(int), 1, stream);
				file_pointer += sizeof(int);
				fseek(stream, file_pointer, SEEK_SET);
				break;
			case SD:
				len = CAPE_WORD;
				break;
			case MD:
				break;
		} 
  		if (len <= 0)
  			continue;

  		buff = (unsigned char *) malloc(len);
  		if (buff == NULL) {
  			rc = 1;
  			break;
  		}
  		if (fread(buff, len, 1, stream) != 1) {
  			free(buff);
  			rc = 1;
  			break;
  		}
  		file_pointer +=len;
  		fseek(stream, file_pointer, SEEK_SET);  		
 		
  		rc = ioctl_write_data(child_id, buff, addr, len);
  		free(buff);
  		if (rc != 0) {
  			fprintf(stderr,
  				"Monitor %ld: failed to inject checkpoint data at 0x%lx len=%d: %s\n",
  				node, addr, len, strerror(errno));
  			break;
  		}
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
					
	//This part will act on data, depends on its properties
	if(tmp->addr == pt->addr){
		free(pt);
		pt = plist;
		if ((prop->properties == D_LAST_PRIVATE) || 
				(prop->properties == D_SHARED) ||
				(prop->properties == D_COPY_IN))
		{
			memcpy(tmp->data, pt->data, CAPE_WORD);
			return 0;
		}
		
		//D_REDUCTION_SUM
		if (prop->properties == D_REDUCTION_SUM){
			if(prop->datatype == CAPE_FLOAT){
				float f=0.0, f1=0.0, f2=0.0;
				memcpy(&f1,tmp->data, CAPE_WORD);
				memcpy(&f2,pt->data, CAPE_WORD);
				f = f1 + f2;
				memcpy(tmp->data, &f, CAPE_WORD);											

				return 0;
			}
			if((prop->datatype == CAPE_INT) || (prop->properties = CAPE_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(int));
				memcpy(&f2,pt->data, sizeof(int));
				f = f1 + f2;											
				memcpy(tmp->data, &f, sizeof(int));
				//printf("\n Sum - int = %d",f); 
				return 0;
			}
			if((prop->datatype == CAPE_UNSIGNED_INT) || (prop->properties = CAPE_UNSIGNED_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned int));
				memcpy(&f2,pt->data, sizeof(unsigned int));
				f = f1 + f2;
				memcpy(tmp->data, &f, sizeof(unsigned int));
				//printf("\n Sum - unsigned int = %ld",f); 
				return 0;
			}
			if((prop->datatype == CAPE_BYTE) || (prop->properties = CAPE_CHAR))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(char));
				memcpy(&f2,pt->data, sizeof(char));
				f = f1 + f2;
				memcpy(tmp->data, &f, sizeof(char));
				return 0;
			}
			if(prop->datatype == CAPE_UNSIGNED_CHAR) 
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned char));
				memcpy(&f2,pt->data, sizeof(unsigned char));
				f = f1 + f2;
				memcpy(tmp->data, &f, sizeof(unsigned char));
				return 0;
			}
			return 1; //error
		}//end if D_REDUCTION_SUM	
		
		//D_REDUCTION_MUL
		if (prop->properties == D_REDUCTION_MUL){
			if(prop->datatype == CAPE_FLOAT){
				float f=0.0, f1=0.0, f2=0.0;
				memcpy(&f1,tmp->data, CAPE_WORD);
				memcpy(&f2,pt->data, CAPE_WORD);
				f = f1 * f2;
				memcpy(tmp->data, &f, CAPE_WORD);											
				//printf("\n Sum = %f",f); 
				return 0;
			}
			if((prop->datatype == CAPE_INT) || (prop->properties = CAPE_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(int));
				memcpy(&f2,pt->data, sizeof(int));
				f = f1 * f2;											
				memcpy(tmp->data, &f, sizeof(int));
				//printf("\n Sum - int = %d",f); 
				return 0;
			}
			if((prop->datatype == CAPE_UNSIGNED_INT) || (prop->properties = CAPE_UNSIGNED_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned int));
				memcpy(&f2,pt->data, sizeof(unsigned int));
				f = f1 * f2;
				memcpy(tmp->data, &f, sizeof(unsigned int));
				//printf("\n Sum - unsigned int = %ld",f); 
				return 0;
			}
			if((prop->datatype == CAPE_BYTE) || (prop->properties = CAPE_CHAR))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(char));
				memcpy(&f2,pt->data, sizeof(char));
				f = f1 * f2 ;
				memcpy(tmp->data, &f, sizeof(char));
				return 0;
			}
			if(prop->datatype == CAPE_UNSIGNED_CHAR) 
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned char));
				memcpy(&f2,pt->data, sizeof(unsigned char));
				f = f1 * f2;
				memcpy(tmp->data, &f, sizeof(unsigned char));
				return 0;
			}
			return 1; //error
		}//end if D_REDUCTION_MUL
		
		
		//D_REDUCTION_MAX
		if (prop->properties == D_REDUCTION_MAX){
			if(prop->datatype == CAPE_FLOAT){
				float f=0.0, f1=0.0, f2=0.0;
				memcpy(&f1,tmp->data, CAPE_WORD);
				memcpy(&f2,pt->data, CAPE_WORD);
				f = (f1 >= f2)? f1 : f2 ;
				memcpy(tmp->data, &f, CAPE_WORD);											
				//printf("\n Sum = %f",f); 
				return 0;
			}
			if((prop->datatype == CAPE_INT) || (prop->properties = CAPE_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(int));
				memcpy(&f2,pt->data, sizeof(int));
				f = (f1 >= f2)? f1 : f2 ;											
				memcpy(tmp->data, &f, sizeof(int));
				//printf("\n Sum - int = %d",f); 
				return 0;
			}
			if((prop->datatype == CAPE_UNSIGNED_INT) || (prop->properties = CAPE_UNSIGNED_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned int));
				memcpy(&f2,pt->data, sizeof(unsigned int));
				f = (f1 >= f2)? f1 : f2 ;
				memcpy(tmp->data, &f, sizeof(unsigned int));
				//printf("\n Sum - unsigned int = %ld",f); 
				return 0;
			}
			if((prop->datatype == CAPE_BYTE) || (prop->properties = CAPE_CHAR))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(char));
				memcpy(&f2,pt->data, sizeof(char));
				f = (f1 >= f2)? f1 : f2 ;
				memcpy(tmp->data, &f, sizeof(char));
				return 0;
			}
			if(prop->datatype == CAPE_UNSIGNED_CHAR) 
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned char));
				memcpy(&f2,pt->data, sizeof(unsigned char));
				f = (f1 >= f2)? f1 : f2 ;
				memcpy(tmp->data, &f, sizeof(unsigned char));
				return 0;
			}
			return 1; //error
		}//end if D_REDUCTION_MAX
		
		
		//D_REDUCTION_MIN
		if (prop->properties == D_REDUCTION_MIN){
			if(prop->datatype == CAPE_FLOAT){
				float f=0.0, f1=0.0, f2=0.0;
				memcpy(&f1,tmp->data, CAPE_WORD);
				memcpy(&f2,pt->data, CAPE_WORD);
				f = (f1 >= f2)? f2 : f1 ;
				memcpy(tmp->data, &f, CAPE_WORD);											
				//printf("\n Sum = %f",f); 
				return 0;
			}
			if((prop->datatype == CAPE_INT) || (prop->properties = CAPE_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(int));
				memcpy(&f2,pt->data, sizeof(int));
				f = (f1 >= f2)? f2 : f1 ;											
				memcpy(tmp->data, &f, sizeof(int));
				//printf("\n Sum - int = %d",f); 
				return 0;
			}
			if((prop->datatype == CAPE_UNSIGNED_INT) || (prop->properties = CAPE_UNSIGNED_LONG))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned int));
				memcpy(&f2,pt->data, sizeof(unsigned int));
				f = (f1 >= f2)? f2 : f1 ;
				memcpy(tmp->data, &f, sizeof(unsigned int));
				//printf("\n Sum - unsigned int = %ld",f); 
				return 0;
			}
			if((prop->datatype == CAPE_BYTE) || (prop->properties = CAPE_CHAR))
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(char));
				memcpy(&f2,pt->data, sizeof(char));
				f = (f1 >= f2)? f2 : f1 ;
				memcpy(tmp->data, &f, sizeof(char));
				return 0;
			}
			if(prop->datatype == CAPE_UNSIGNED_CHAR) 
			{
				int f=0, f1=0, f2=0;
				memcpy(&f1,tmp->data, sizeof(unsigned char));
				memcpy(&f2,pt->data, sizeof(unsigned char));
				f = (f1 >= f2)? f2 : f1 ;
				memcpy(tmp->data, &f, sizeof(unsigned char));
				return 0;
			}
			return 1; //error
		}//end if D_REDUCTION_MIN
	
		return 1; //error
	}//end if tmp->addr = pt->addr	

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
	
	current_ckpt_struct = SSD;
	
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
		rc = merge_bitmap_sections(total_ckpt_stream, &src_bmp, &tmp_bmp);
		if (rc != 0)
			goto done;

		//Write L2 into Total_ckpt
		merge_data(src_read_stream, src_ckpt_data, src_ckpt_size, src_bmp.end, FINAL_CHECKPOINT);

		//Write L1 into Total_ckpt
		merge_data(tmp_read_stream, tmp_ckpt, tmp_size, tmp_bmp.end, TOTAL_CHECKPOINT);

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

		//Write L1 into Total_ckpt
		merge_data(tmp_read_stream, tmp_ckpt, tmp_size, tmp_bmp.end, TOTAL_CHECKPOINT);

		//Write L2 into Total_ckpt
		merge_data(src_read_stream, src_ckpt_data, src_ckpt_size, src_bmp.end, FINAL_CHECKPOINT);
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
/* Master receives all slaves' checkpoints in parallel and synchronously
 * folds each one into its in-memory total checkpoint as soon as the
 * payload arrives. Slaves transmit concurrently; UCX progress drives
 * every recv slot inside one worker thread.
 *
 * Memory cap: at most CAPE_MS_MAX_QUEUE_BYTES of unmerged payloads are
 * resident at once. A slot that has received its size header but would
 * overflow the cap blocks in CAPE_MS_HAVE_SIZE; the data recv is only
 * posted after the merge consumes other payloads. The slave's send
 * blocks until we post the recv, so back-pressure flows the right way. */
int require_waitfor_checkpoint() {
	int rc = 0;
	int n_slaves = num_nodes - 1;
	int merged_count = 0;
	size_t queue_bytes = 0;
	struct cape_ms_slot *slots;
	int i;

	if (n_slaves <= 0) {
		/* Single-node run: nothing to wait for. */
		goto fold_master;
	}

	slots = calloc((size_t)n_slaves, sizeof(*slots));
	if (slots == NULL)
		return 1;

	/* Pre-post one size-header recv per slave for maximum ingress
	 * parallelism. UCX matches each by source rank via the tag. */
	for (i = 0; i < n_slaves; ++i) {
		slots[i].sender       = i + 1;
		slots[i].state        = CAPE_MS_RECV_SIZE;
		slots[i].payload_size = 0;
		slots[i].size_req     = cape_ms_post_recv(
			&slots[i].payload_size, sizeof(slots[i].payload_size),
			slots[i].sender, TAG_CKPT_DATA);
	}

	while (merged_count < n_slaves) {
		ucp_worker_progress(ucp_worker);
		CAPE_PROFILE_INC(ucx_progress_calls);

		for (i = 0; i < n_slaves; ++i) {
			struct cape_ms_slot *s = &slots[i];

			if (s->state == CAPE_MS_RECV_SIZE &&
			    cape_ms_req_done(s->size_req)) {
				cape_ms_req_release(s->size_req);
				s->size_req = NULL;
				if (s->payload_size <= 0) {
					fprintf(stderr,
						"Monitor %ld: bad ckpt size %d from slave %d\n",
						node, s->payload_size, s->sender);
					rc = 1;
					goto cleanup;
				}
				s->state = CAPE_MS_HAVE_SIZE;
			}

			if (s->state == CAPE_MS_HAVE_SIZE &&
			    queue_bytes + (size_t)s->payload_size
			    <= CAPE_MS_MAX_QUEUE_BYTES) {
				s->payload = malloc((size_t)s->payload_size);
				if (s->payload == NULL) {
					rc = 1;
					goto cleanup;
				}
				queue_bytes += (size_t)s->payload_size;
				s->data_req = cape_ms_post_recv(
					s->payload, (size_t)s->payload_size,
					s->sender, TAG_CKPT_DATA + 1);
				s->state = CAPE_MS_RECV_DATA;
			}

			if (s->state == CAPE_MS_RECV_DATA &&
			    cape_ms_req_done(s->data_req)) {
				cape_ms_req_release(s->data_req);
				s->data_req = NULL;
				s->state = CAPE_MS_DONE;
			}

			if (s->state == CAPE_MS_DONE) {
				/* Synchronous merge into total_ckpt — single
				 * injector, so order is well-defined and the
				 * existing merge_external_checkpoint
				 * tie-breaking (by timespan) still holds. */
				FILE *stream = fmemopen(s->payload,
							(size_t)s->payload_size,
							"rb");
				if (stream == NULL) {
					rc = 1;
					goto cleanup;
				}
				rc = merge_external_checkpoint(
					stream, s->payload,
					(size_t)s->payload_size);
				fclose(stream);
				free(s->payload);
				queue_bytes -= (size_t)s->payload_size;
				s->payload = NULL;
				s->payload_size = 0;
				s->state = CAPE_MS_IDLE;
				merged_count++;
				if (rc != 0)
					goto cleanup;
			}
		}
	}

cleanup:
	for (i = 0; i < n_slaves; ++i) {
		if (slots[i].size_req)
			cape_ms_req_release(slots[i].size_req);
		if (slots[i].data_req)
			cape_ms_req_release(slots[i].data_req);
		free(slots[i].payload);
	}
	free(slots);
	if (rc != 0) {
		printf("Monitor %ld: Error on require_waitfor_checkpoint\n", node);
		return rc;
	}

fold_master:
	/* Fold master's own checkpoint into the merged total so the
	 * broadcast-back covers everyone's changes. */
	if (final_ckpt_size > 0) {
		FILE *stream = fmemopen(final_ckpt, final_ckpt_size, "rb");
		if (stream != NULL) {
			rc = merge_external_checkpoint(stream, final_ckpt,
						       final_ckpt_size);
			fclose(stream);
		}
	}

	dprintf("Monitor %ld: master merged %d slave ckpts + own; total_ckpt_size=%zu\n",
		node, merged_count, total_ckpt_size);
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
#ifdef USE_UCC_BCAST
	unsigned char *src_buf = NULL;
	size_t src_size = 0;

	if (node == 0) {
		/* Master broadcasts the merged total. Fall back to final_ckpt
		 * only if total_ckpt is empty (single-node run with no merges). */
		if (total_ckpt_size > 0) {
			src_buf  = total_ckpt;
			src_size = total_ckpt_size;
		} else {
			src_buf  = final_ckpt;
			src_size = final_ckpt_size;
		}
		if (src_size > (size_t)INT_MAX) {
			fprintf(stderr,
				"Monitor %ld: checkpoint too large for int size header: %zu\n",
				node, src_size);
			return 1;
		}
		buffer_size = (int)src_size;
		buffer_ckpt = src_buf;
	} else {
		buffer_size = 0;
		buffer_ckpt = NULL;
	}

	rc = cape_ucc_bcast(&buffer_size, 1, UCC_DT_INT32,
			    "broadcast checkpoint size");
	if (rc != 0)
		return rc;
	if (buffer_size < 0) {
		fprintf(stderr, "Monitor %ld: invalid broadcast checkpoint size %d\n",
			node, buffer_size);
		return 1;
	}

	if (node != 0) {
		buffer_ckpt = NULL;
		if (buffer_size > 0) {
			buffer_ckpt = malloc((size_t)buffer_size);
			if (buffer_ckpt == NULL)
				return 1;
		}
	}

	rc = cape_ucc_bcast(buffer_ckpt, (size_t)buffer_size, UCC_DT_INT8,
			    "broadcast checkpoint data");
	if (rc != 0) {
		if (node != 0)
			free(buffer_ckpt);
		return rc;
	}

	if (node != 0) {
		after_ckpt_stream = open_binary_memstream(&after_ckpt,
							  &after_ckpt_size);
		if (after_ckpt_stream == NULL) {
			free(buffer_ckpt);
			return 1;
		}
		if (buffer_size > 0 &&
		    fwrite(buffer_ckpt, (size_t)buffer_size, 1,
			   after_ckpt_stream) != 1) {
			free(buffer_ckpt);
			return 1;
		}
		fflush(after_ckpt_stream);
		free(buffer_ckpt);
		buffer_ckpt = NULL;
	}
#else
	if(node==0)
	{
		/* Master ships the merged total. Fall back to final_ckpt only
		 * if total_ckpt is empty (single-node run with no merges). */
		unsigned char *src_buf;
		size_t         src_size;
		if (total_ckpt_size > 0) {
			src_buf  = total_ckpt;
			src_size = total_ckpt_size;
		} else {
			src_buf  = final_ckpt;
			src_size = final_ckpt_size;
		}
		buffer_size = (int)src_size;
		buffer_ckpt = src_buf;
		for(i = 1; i < num_nodes; i++){
			cape_ucx_send(&buffer_size, sizeof(int), i, TAG_BCAST_SIZE);
			cape_ucx_send(buffer_ckpt, buffer_size, i, TAG_BCAST_DATA);
		}
	}
	else
	{
		/* Slaves receive size and data from master */
		cape_ucx_recv(&buffer_size, sizeof(int), 0, TAG_BCAST_SIZE);
		buffer_ckpt = malloc(buffer_size);
		cape_ucx_recv(buffer_ckpt, buffer_size, 0, TAG_BCAST_DATA);
		//open the stream memory file
		after_ckpt_stream = open_binary_memstream(&after_ckpt, &after_ckpt_size);
		fwrite(buffer_ckpt, buffer_size, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		free(buffer_ckpt);
	}
#endif

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
#if 0 /* unused helper */
unsigned int nearest_power_of_two(unsigned int n){
	
	if (is_power_of_two(n)) return n;
	while(n > 1){
		n--;
		if (is_power_of_two(n)) return n;
	}
	return 0;
}
#endif

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
		uint32_t token_size = TAG_ALLREDUCE_BASE + (i * 2);
		uint32_t token_data = TAG_ALLREDUCE_BASE + (i * 2) + 1;

		//send size of message
		cape_ucx_sendrecv(&message_size, sizeof(int), right,
						  &recv_message_size, sizeof(int), left,
						  token_size);

		recv_buffer = malloc(sizeof(char) * recv_message_size) ;

		//send data
		cape_ucx_sendrecv(send_buffer, message_size, right,
						  recv_buffer, recv_message_size, left,
						  token_data);


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
		uint32_t token_size = TAG_ALLREDUCE_BASE + (i * 2);
		uint32_t token_data = TAG_ALLREDUCE_BASE + (i * 2) + 1;

		partner = node ^ (1 << i);



		//send size of message
		cape_ucx_sendrecv(&send_msg_size, sizeof(int), partner,
						  &recv_msg_size, sizeof(int), partner,
						  token_size);



		recv_msg = malloc(sizeof(char) * recv_msg_size) ;

		//send data
		cape_ucx_sendrecv(send_msg, send_msg_size, partner,
						  recv_msg, recv_msg_size, partner,
						  token_data);

		ckpt_stream = open_binary_memstream(&ckpt_data, &ckpt_size);
		fwrite(recv_msg, recv_msg_size, 1, ckpt_stream);
		fflush(ckpt_stream);

		rc = merge_external_checkpoint(ckpt_stream, ckpt_data, ckpt_size);
		join_checkpoint(TOTAL_CHECKPOINT, final_list_ckpt_head);

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
	final_list_ckpt_head = list_ckpt_head;
	final_list_ckpt_tail = list_ckpt_tail;
	rc=  merge_external_checkpoint(final_ckpt_stream, final_ckpt, final_ckpt_size);		
	join_checkpoint(TOTAL_CHECKPOINT, final_list_ckpt_head);
	fclose(final_ckpt_stream);
	/* open_memstream's buffer survives fclose — caller must free it.
	 * Without this the per-phase ckpt buffer accumulates and the
	 * monitor's RSS grows as O(phases × peers × dirty_per_phase),
	 * which OOMs at large node counts. */
	free(final_ckpt);
	final_ckpt = NULL;
	final_ckpt_size = 0;
	if (is_power_of_two(num_nodes) && (total_ckpt_size < LARGE_CHECKPOINT)){
	    rc = hypercube_allreduce();
	}	
	else {
		rc = ring_allreduce();
		 
	}	
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
		
