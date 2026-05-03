#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../../include/cape.h"
#include <ucp/api/ucp.h>
#ifdef USE_PMIX
#include <pmix.h>
#endif
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#ifdef CAPE_COMPRESS
#include <zlib.h>
#endif

#if DDEBUG
#define dprintf(fmt, args...) printf(fmt, ## args);
#else
#define dprintf(fmt, args...) ;
#endif
/* ====================================================================
 * Per-node performance profiling  (compile with -DCAPE_PROFILE to enable)
 * ==================================================================== */
#ifdef CAPE_PROFILE

static inline double cape_get_ns(void) {
    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    return (double)_ts.tv_sec * 1e9 + (double)_ts.tv_nsec;
}

static inline double cape_get_realtime_ns(void) {
    struct timespec _ts;
    clock_gettime(CLOCK_REALTIME, &_ts);
    return (double)_ts.tv_sec * 1e9 + (double)_ts.tv_nsec;
}

static double prof_ckpt_start_ns      = 0.0;
static double prof_generate_ckpt_ns   = 0.0;
static double prof_allreduce_total_ns = 0.0;
static double prof_allreduce_size_ns  = 0.0;
static double prof_allreduce_data_ns  = 0.0;
static double prof_merge_ckpt_ns      = 0.0;
static double prof_inject_ckpt_ns     = 0.0;
static double prof_ucx_recv_wait_ns   = 0.0;
static double prof_ucx_send_wait_ns   = 0.0;
static double prof_cape_init_ns       = 0.0;
static double prof_ucx_config_ns      = 0.0;
static double prof_ucp_init_ns        = 0.0;
static double prof_ucp_worker_ns      = 0.0;
static double prof_ucp_worker_addr_ns = 0.0;
static double prof_ucx_bootstrap_fs_ns        = 0.0;
static double prof_ucx_bootstrap_fs_addr_ns   = 0.0;
static double prof_ucx_bootstrap_fs_rdy_ns    = 0.0;
static double prof_ucx_bootstrap_fs_wait_ns   = 0.0;
static double prof_ucx_bootstrap_fs_read_ns   = 0.0;
static double prof_ucx_bootstrap_ep_create_ns = 0.0;
static double prof_ucx_bootstrap_pmix_ns      = 0.0;
static double prof_ucx_bootstrap_pmix_put_ns  = 0.0;
static double prof_ucx_bootstrap_pmix_fence_ns = 0.0;
static double prof_ucx_bootstrap_pmix_get_ns  = 0.0;

static unsigned long prof_ckpt_start_count    = 0;
static unsigned long prof_generate_ckpt_count = 0;
static unsigned long prof_allreduce_count     = 0;
static unsigned long prof_inject_ckpt_count   = 0;
static unsigned long prof_sendrecv_count      = 0;
static unsigned long prof_bytes_sent          = 0;
static unsigned long prof_bytes_received      = 0;
static unsigned long prof_ucx_bootstrap_ep_create_count = 0;
static unsigned long prof_ucx_bootstrap_fs_wait_iters   = 0;

/* ---- Wider region profiling ------------------------------------------ */
static double prof_cape_begin_ns      = 0.0;
static double prof_cape_end_ns        = 0.0;   /* full cape_end wall time */
static double prof_ckpt_stop_ns       = 0.0;
static double prof_release_ckpt_ns    = 0.0;
static double prof_sync_ckpt_ns       = 0.0;   /* allreduce + inject wall */
static double prof_compute_ns         = 0.0;   /* between ckpt_start and cape_end */
static double prof_declare_var_ns     = 0.0;
static double prof_enter_exit_func_ns = 0.0;

static unsigned long prof_cape_begin_count   = 0;
static unsigned long prof_cape_end_count     = 0;
static unsigned long prof_ckpt_stop_count    = 0;
static unsigned long prof_release_count      = 0;
static unsigned long prof_sync_count         = 0;
static unsigned long prof_compute_count      = 0;
static unsigned long prof_declare_var_count  = 0;
static unsigned long prof_enter_exit_count   = 0;

/* Mark when parallel compute starts (ckpt_start returns or cape_begin
 * finishes) so cape_end can attribute elapsed time to the user's compute. */
static double prof_compute_start_ns   = 0.0;
static int    prof_compute_started    = 0;

#define CAPE_PROFILE_MAX_STEPS    128
#define CAPE_PROFILE_MAX_ARRIVALS 64

enum {
	CAPE_SR_PHASE_NONE = 0,
	CAPE_SR_PHASE_SIZE = 1,
	CAPE_SR_PHASE_DATA = 2
};

static double prof_step_size_call_ns[CAPE_PROFILE_MAX_STEPS];
static double prof_step_data_call_ns[CAPE_PROFILE_MAX_STEPS];
static double prof_step_size_recv_wait_ns[CAPE_PROFILE_MAX_STEPS];
static double prof_step_data_recv_wait_ns[CAPE_PROFILE_MAX_STEPS];
static double prof_step_size_max_recv_wait_ns[CAPE_PROFILE_MAX_STEPS];
static double prof_step_data_max_recv_wait_ns[CAPE_PROFILE_MAX_STEPS];
static uint32_t prof_step_size_max_epoch[CAPE_PROFILE_MAX_STEPS];
static uint32_t prof_step_data_max_epoch[CAPE_PROFILE_MAX_STEPS];
static unsigned long prof_step_size_count[CAPE_PROFILE_MAX_STEPS];
static unsigned long prof_step_data_count[CAPE_PROFILE_MAX_STEPS];
static int prof_step_partner[CAPE_PROFILE_MAX_STEPS];
static unsigned char prof_step_partner_set[CAPE_PROFILE_MAX_STEPS];
static unsigned int prof_step_max_seen = 0;
static unsigned long prof_step_overflow = 0;

static int prof_sr_phase = CAPE_SR_PHASE_NONE;
static int prof_sr_step = -1;
static int prof_sr_partner = -1;
static uint32_t prof_sr_epoch = 0;

static double prof_allreduce_arrival_first_rt_ns = 0.0;
static double prof_allreduce_arrival_last_rt_ns = 0.0;
static double prof_allreduce_arrival_rt_ns[CAPE_PROFILE_MAX_ARRIVALS];
static uint32_t prof_allreduce_arrival_epoch[CAPE_PROFILE_MAX_ARRIVALS];
static unsigned long prof_allreduce_arrival_count = 0;

static inline void cape_prof_set_sendrecv_context(int phase, int step, int partner, uint32_t epoch)
{
	prof_sr_phase = phase;
	prof_sr_step = step;
	prof_sr_partner = partner;
	prof_sr_epoch = epoch;
}

static inline void cape_prof_clear_sendrecv_context(void)
{
	prof_sr_phase = CAPE_SR_PHASE_NONE;
	prof_sr_step = -1;
	prof_sr_partner = -1;
	prof_sr_epoch = 0;
}

static inline void cape_prof_acc_sendrecv(double call_ns, double recv_wait_ns)
{
	unsigned int s;

	if (prof_sr_step < 0)
		return;
	if ((unsigned int)prof_sr_step >= CAPE_PROFILE_MAX_STEPS) {
		prof_step_overflow++;
		return;
	}

	s = (unsigned int)prof_sr_step;
	if (!prof_step_partner_set[s]) {
		prof_step_partner[s] = prof_sr_partner;
		prof_step_partner_set[s] = 1;
	} else if (prof_step_partner[s] != prof_sr_partner) {
		prof_step_partner[s] = -2; /* mixed partner set for this step index */
	}
	if (s + 1 > prof_step_max_seen)
		prof_step_max_seen = s + 1;

	if (prof_sr_phase == CAPE_SR_PHASE_SIZE) {
		prof_step_size_call_ns[s] += call_ns;
		prof_step_size_recv_wait_ns[s] += recv_wait_ns;
		prof_step_size_count[s]++;
		if (recv_wait_ns > prof_step_size_max_recv_wait_ns[s]) {
			prof_step_size_max_recv_wait_ns[s] = recv_wait_ns;
			prof_step_size_max_epoch[s] = prof_sr_epoch;
		}
	} else if (prof_sr_phase == CAPE_SR_PHASE_DATA) {
		prof_step_data_call_ns[s] += call_ns;
		prof_step_data_recv_wait_ns[s] += recv_wait_ns;
		prof_step_data_count[s]++;
		if (recv_wait_ns > prof_step_data_max_recv_wait_ns[s]) {
			prof_step_data_max_recv_wait_ns[s] = recv_wait_ns;
			prof_step_data_max_epoch[s] = prof_sr_epoch;
		}
	}
}

#define PROF_START(var)       double var = cape_get_ns()
#define PROF_ACC(accum, var)  (accum) += cape_get_ns() - (var)
#define PROF_INC(counter)     (counter)++
#define PROF_ADD(counter, v)  (counter) += (v)
#define PROF_SET_SENDRECV_CONTEXT(phase, step, partner, epoch) \
	cape_prof_set_sendrecv_context((phase), (step), (partner), (epoch))
#define PROF_CLEAR_SENDRECV_CONTEXT() cape_prof_clear_sendrecv_context()
#define PROF_RECORD_RECV_WAIT(delta_var, start_var) \
	double delta_var = cape_get_ns() - (start_var); \
	prof_ucx_recv_wait_ns += (delta_var)
#define PROF_RECORD_SEND_WAIT(start_var) \
	PROF_ACC(prof_ucx_send_wait_ns, start_var)
#define PROF_RECORD_SENDRECV(start_var, recv_wait_ns) \
	cape_prof_acc_sendrecv(cape_get_ns() - (start_var), (recv_wait_ns))
#define PROF_RECORD_ALLREDUCE_ARRIVAL(epoch) \
	do { \
		double _arrival_rt_ns = cape_get_realtime_ns(); \
		if (prof_allreduce_arrival_count == 0) \
			prof_allreduce_arrival_first_rt_ns = _arrival_rt_ns; \
		prof_allreduce_arrival_last_rt_ns = _arrival_rt_ns; \
		if (prof_allreduce_arrival_count < CAPE_PROFILE_MAX_ARRIVALS) { \
			unsigned long idx = prof_allreduce_arrival_count; \
			prof_allreduce_arrival_rt_ns[idx] = _arrival_rt_ns; \
			prof_allreduce_arrival_epoch[idx] = (epoch); \
		} \
		prof_allreduce_arrival_count++; \
	} while (0)

#else /* !CAPE_PROFILE */

#define PROF_START(var)       (void)0
#define PROF_ACC(accum, var)  (void)0
#define PROF_INC(counter)     (void)0
#define PROF_ADD(counter, v)  (void)0
#define PROF_SET_SENDRECV_CONTEXT(phase, step, partner, epoch) do { } while (0)
#define PROF_CLEAR_SENDRECV_CONTEXT() do { } while (0)
#define PROF_RECORD_RECV_WAIT(delta_var, start_var) double delta_var = 0.0
#define PROF_RECORD_SEND_WAIT(start_var) do { } while (0)
#define PROF_RECORD_SENDRECV(start_var, recv_wait_ns) do { } while (0)
#define PROF_RECORD_ALLREDUCE_ARRIVAL(epoch) do { } while (0)

#endif /* CAPE_PROFILE */
/* ==================================================================== */

/**********Local Variables ********************************************/
static char *_var_data; //copy variable data
static unsigned long * __var_addr; //address of variables
static unsigned long * __var_len; //
static FILE * __ckpt_data;


PointerList *__var_heap_list_head = NULL;
PointerList *__var_heap_list_tail = NULL;

VarList *__active_variable_head = NULL;
VarList *__active_variable_tail = NULL;
VarList *__var_list_head = NULL;
VarList *__var_list_tail = NULL;

/* Hash table heads for O(1) lookups */
static VarList *__var_list_hash = NULL;         /* hash by (addr,level) for shared vars */
static VarList *__active_var_hash = NULL;       /* hash by (addr,level) for active vars */
static PointerList *__heap_hash_mgr = NULL;     /* hash by manager_addr */

FILE *ckpt_data_stream;
FILE *before_ckpt_stream, *after_ckpt_stream, *final_ckpt_stream;
char *ckpt_data, *before_ckpt, *after_ckpt, *final_ckpt;
size_t ckpt_data_size, before_ckpt_size, after_ckpt_size, final_ckpt_size;

/* Manual byte buffer capacity for after_ckpt. */
static size_t after_ckpt_cap = 0;

/* ===== bitmap S-section format =====
 *   Each variable in the S section is encoded as:
 *     [var_addr:8][var_size:8][n_pages:4][n_dirty_pages:4]
 *     [page_bmp: ceil(n_pages/8) bytes]   -- 1 bit per page
 *     For each dirty page in page_bmp scan order:
 *       [word_bmp:128 bytes]              -- 1 bit per 4-byte word (1024 bits)
 *       [page_data: BMP_PAGE_SIZE bytes]   -- full page contents
 *
 *   Merging two such sections: page_bmp_out = A | B. For pages dirty in
 *   only one side, copy that side's word_bmp + data. For pages dirty in
 *   both, OR the word bitmaps and merge the data word-by-word (later
 *   write wins by timestamp at the section level).
 *
 *   Inject: for each dirty page, walk the word_bmp and write only the
 *   words whose bit is set — keeps cache lines clean, page-aligned. */
#define BMP_PAGE_SIZE       4096
#define BMP_PAGE_SHIFT      12
#define BMP_WORDS_PER_PAGE  (BMP_PAGE_SIZE / CAPE_WORD)   /* 1024 */
#define BMP_WORD_BMP_BYTES  (BMP_WORDS_PER_PAGE / 8)      /* 128  */
#define BMP_PAGE_ENTRY_BYTES (BMP_WORD_BMP_BYTES + BMP_PAGE_SIZE)  /* 4224 */

static inline int bmp_get(const unsigned char *bmp, size_t i) {
	return (bmp[i >> 3] >> (i & 7)) & 1;
}
static inline void bmp_set(unsigned char *bmp, size_t i) {
	bmp[i >> 3] |= (unsigned char)(1u << (i & 7));
}
/* Round var size up to a whole page so n_pages is unambiguous. */
static inline size_t bmp_round_pages(size_t bytes) {
	return (bytes + BMP_PAGE_SIZE - 1) >> BMP_PAGE_SHIFT;
}

/* Allocate after_ckpt to an EXACT size — used by merge_checkpoint where
 * we know the worst-case output bound. The doubling reserve below
 * over-allocates by up to 2x when the request crosses a power of 2,
 * which can OOM on big merges. */
static inline void after_ckpt_reserve_exact(size_t need)
{
	if (after_ckpt) {
		free(after_ckpt);
		after_ckpt = NULL;
	}
	after_ckpt_cap = need ? need : 1;
	after_ckpt_size = 0;
	after_ckpt = (char *)malloc(after_ckpt_cap);
}

static inline void after_ckpt_reserve(size_t need)
{
	if (need <= after_ckpt_cap)
		return;
	size_t new_cap = after_ckpt_cap ? after_ckpt_cap : 4096;
	while (new_cap < need)
		new_cap *= 2;
	after_ckpt = (char *)realloc(after_ckpt, new_cap);
	after_ckpt_cap = new_cap;
}

static inline void after_ckpt_append(const void *src, size_t n)
{
	after_ckpt_reserve(after_ckpt_size + n);
	memcpy(after_ckpt + after_ckpt_size, src, n);
	after_ckpt_size += n;
}

static inline void after_ckpt_patch(size_t off, const void *src, size_t n)
{
	memcpy(after_ckpt + off, src, n);
}

static inline void after_ckpt_release(void)
{
	after_ckpt_release();
	after_ckpt_cap = 0;
}

static unsigned char __activate_func_level__ = 1;
static unsigned char __parallel_level__ = 0;
static int __node__ = -1; //current node
static int __nnodes__ = -1 ; // Number of working nodes
static int __total_nodes__ = -1 ; //Total nodes in the system
static int __cape_token__ = -1;
static unsigned int __current_session__ = -1;
static uint32_t __allreduce_epoch__ = 0;

static int __is_inside_parallel_region__ = 0;

char buffer[4096];
char __ckpt_data_file[100];
int __ckpt_data_size = 0;

#ifdef CAPE_PROFILE
static int cape_ucx_diag_configured = 0;
static int cape_ucx_diag_enabled = 0;
static double cape_ucx_diag_slow_ns = 1000.0 * 1e6;

static const char *cape_env_or_dash(const char *name)
{
	const char *value = getenv(name);
	return (value && value[0]) ? value : "-";
}

static void cape_ucx_diag_configure(void)
{
	const char *enabled;
	const char *slow_ms;
	double ms;

	if (cape_ucx_diag_configured)
		return;
	cape_ucx_diag_configured = 1;

	enabled = getenv("CAPE_UCX_DIAG");
	if (enabled && enabled[0] &&
	    strcmp(enabled, "0") != 0 &&
	    strcmp(enabled, "false") != 0 &&
	    strcmp(enabled, "no") != 0) {
		cape_ucx_diag_enabled = 1;
	}

	slow_ms = getenv("CAPE_UCX_DIAG_SLOW_MS");
	if (slow_ms && slow_ms[0]) {
		ms = atof(slow_ms);
		if (ms > 0.0)
			cape_ucx_diag_slow_ns = ms * 1e6;
	}
}

static int cape_ucx_diag_on(void)
{
	cape_ucx_diag_configure();
	return cape_ucx_diag_enabled;
}

static int cape_ucx_diag_is_slow(double ns)
{
	cape_ucx_diag_configure();
	return cape_ucx_diag_enabled && ns >= cape_ucx_diag_slow_ns;
}

static const char *cape_ucx_diag_phase_name(int phase)
{
	if (phase == CAPE_SR_PHASE_SIZE)
		return "size";
	if (phase == CAPE_SR_PHASE_DATA)
		return "data";
	return "none";
}

static void cape_ucx_diag_log(const char *fmt, ...)
{
	va_list ap;

	if (!cape_ucx_diag_on())
		return;
	fprintf(stderr, "[CAPE UCX DIAG] node=%d/%d ", __node__, __nnodes__);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#endif

/**********UCX + PMIx State ********************************************/
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
    ucp_tag_t    sender_tag;   /* actual tag of matched message */
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

/* Block until a non-blocking UCX request finishes.
 * out_tag: if non-NULL, receives the sender_tag from the matched message. */
static void cape_ucx_wait(void *req, size_t expect_len, int check_len,
                          ucp_tag_t *out_tag)
{
    if (out_tag) *out_tag = 0;
    if (req == NULL)
        return; /* completed immediately */
    if (UCS_PTR_IS_ERR(req)) {
        fprintf(stderr, "CAPE UCX error: %s\n",
                ucs_status_string(UCS_PTR_STATUS(req)));
        exit(1);
    }
	cape_ucx_req_t *r = (cape_ucx_req_t *)req;
	while (!r->completed)
		ucp_worker_progress(ucp_worker);
	if (out_tag)
		*out_tag = r->sender_tag;
	if (r->status != UCS_OK) {
		fprintf(stderr, "CAPE UCX request failed: %s\n",
		        ucs_status_string(r->status));
		ucp_request_free(req);
		exit(1);
	}
	if (check_len && (r->recv_len != 0) && (r->recv_len != expect_len)) {
		fprintf(stderr, "CAPE UCX recv length mismatch: got=%zu expected=%zu"
		        " sender_tag=0x%lx\n",
		        r->recv_len, expect_len,
		        (unsigned long)r->sender_tag);
		/* Reset before freeing so reuse from pool starts clean */
		r->completed  = 0;
		r->status     = UCS_OK;
		r->recv_len   = 0;
		r->sender_tag = 0;
		ucp_request_free(req);
		exit(1);
	}
	/* Reset before freeing so reuse from pool starts clean.
	 * UCX's request_init may only be called on first allocation,
	 * not on reuse from the free pool. */
	r->completed  = 0;
	r->status     = UCS_OK;
	r->recv_len   = 0;
	r->sender_tag = 0;
	ucp_request_free(req);
}

/*
 * Simultaneous tag send/receive (mimics MPI_Sendrecv).
 * The UCX tag encodes the sender rank in the upper 32 bits so that a
 * receiver matches only messages from the expected peer.
 */
/*
 * Keep matching bits in the low portion of the tag, which is the safest
 * subset across transports that may not preserve/compare all high tag bits.
 */
#define CAPE_UCX_TAG(sender_rank, token) \
	((uint64_t)((((uint32_t)(sender_rank) & 0x0fffU) << 20) | ((uint32_t)(token) & 0x000fffffU)))
#define CAPE_UCX_TAG_MASK  ((uint64_t)0x00000000ffffffffULL)

static void cape_ucx_sendrecv(
        const void *sendbuf, size_t sendlen, int dest,
        void       *recvbuf, size_t recvlen, int src,
        uint32_t    token)
{
    ucp_tag_t send_tag = CAPE_UCX_TAG(__node__, token);
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src,      token);

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

    // fprintf(stderr, "CAPE DBG node %d: sendrecv send_tag=0x%lx recv_tag=0x%lx"
    //         " sendlen=%zu recvlen=%zu dest=%d src=%d token=0x%x\n",
    //         __node__, (unsigned long)send_tag, (unsigned long)recv_tag,
    //         sendlen, recvlen, dest, src, (unsigned)token);

    /* Post receive before send to avoid lost-message races */
    void *rreq = ucp_tag_recv_nbx(ucp_worker, recvbuf, recvlen,
                                   recv_tag, CAPE_UCX_TAG_MASK, &rp);
    void *sreq = ucp_tag_send_nbx(ucp_endpoints[dest], sendbuf, sendlen,
                                   send_tag, &sp);

    // fprintf(stderr, "CAPE DBG node %d: rreq=%p sreq=%p\n",
    //         __node__, rreq, sreq);

    ucp_tag_t matched_tag = 0;
    PROF_START(_t_sendrecv);
    PROF_START(_t_recv_wait);
    cape_ucx_wait(rreq, recvlen, 1, &matched_tag);
    PROF_RECORD_RECV_WAIT(_recv_wait_ns, _t_recv_wait);
#ifdef CAPE_PROFILE
    if (cape_ucx_diag_is_slow(_recv_wait_ns)) {
        cape_ucx_diag_log(
            "slow_recv_wait_ms=%.3f phase=%s step=%d partner=%d epoch=%u "
            "dest=%d src=%d token=0x%x sendlen=%zu recvlen=%zu "
            "recv_tag=0x%lx matched_tag=0x%lx\n",
            _recv_wait_ns / 1e6,
            cape_ucx_diag_phase_name(prof_sr_phase),
            prof_sr_step, prof_sr_partner, prof_sr_epoch,
            dest, src, (unsigned)token, sendlen, recvlen,
            (unsigned long)recv_tag, (unsigned long)matched_tag);
    }
#endif

    PROF_START(_t_send_wait);
    cape_ucx_wait(sreq, 0, 0, NULL);
#ifdef CAPE_PROFILE
    double _send_wait_ns = cape_get_ns() - _t_send_wait;
    prof_ucx_send_wait_ns += _send_wait_ns;
    if (cape_ucx_diag_is_slow(_send_wait_ns)) {
        cape_ucx_diag_log(
            "slow_send_wait_ms=%.3f phase=%s step=%d partner=%d epoch=%u "
            "dest=%d src=%d token=0x%x sendlen=%zu recvlen=%zu "
            "send_tag=0x%lx\n",
            _send_wait_ns / 1e6,
            cape_ucx_diag_phase_name(prof_sr_phase),
            prof_sr_step, prof_sr_partner, prof_sr_epoch,
            dest, src, (unsigned)token, sendlen, recvlen,
            (unsigned long)send_tag);
    }
#else
    PROF_RECORD_SEND_WAIT(_t_send_wait);
#endif

    PROF_INC(prof_sendrecv_count);
    PROF_ADD(prof_bytes_sent, sendlen);
    PROF_ADD(prof_bytes_received, recvlen);
    PROF_RECORD_SENDRECV(_t_sendrecv, _recv_wait_ns);
}
/***********************************************************************/

/**********Public Variables ********************************************/
int __left__ = -1;
int __right__ = -1;
int __i__;
unsigned long __time_stamp__ = 1 ;
unsigned long __pc__;
/***********************************************************************/


/**********************************************************************/
/* 					Private Functions								 */
/*********************************************************************/
 /*
  * Add active variables in __active_variable list
  * 	Insert at the end of list (LIFO)
  */
int add_active_variable(VarList **vlist_head, VarList **vlist_tail, Var v){
	VarList *vl;
	vl = malloc(sizeof(struct VarList));
	memset(vl, 0, sizeof(struct VarList));
	vl->next = NULL;
	vl->prev = NULL;
	vl->var.addr = v.addr;
	vl->var.size = v.size;
	vl->var.n = v.n;
	vl->var.pro = v.pro;
	vl->var.level = v.level;
	vl->var.dtype = v.dtype;
	vl->var.ispointer = v.ispointer;
	vl->hash_key.addr = v.addr;
	vl->hash_key.level = v.level;
	if (*vlist_head == NULL){
		*vlist_head = vl ;
		*vlist_tail = vl;
		HASH_ADD(hh, __active_var_hash, hash_key, sizeof(VarHashKey), vl);
		return 1;
	}
	VarList *tmp2;
	tmp2 = *vlist_tail;
	//if variable exists in list
	if (tmp2->var.addr == vl->var.addr){
		free(vl);
		return 0;
	}
	//insert at the end of list
	vl->prev = tmp2;
	tmp2->next = vl;
	*vlist_tail = vl;
	HASH_ADD(hh, __active_var_hash, hash_key, sizeof(VarHashKey), vl);
	return 1;
}
/* Forward declarations */
int remove_heap_variables(PointerList **hlist_head, PointerList **hlist_tail, unsigned long manager_addr);
int add_shared_variable(VarList **vlist, VarList **vlist_tail, Var var);
int add_parallel_region(VarList **vlist_head, VarList **vlist_tail, unsigned char level);
void require_generate_checkpoint(char ops_flag);

/*
 * Remove all variables with in var.level =  func_level from activate list
 * and remove all variables on heap that is managered by these variables
 */
int remove_active_variables(VarList **vlist_head, VarList **vlist_tail, unsigned char func_level){

	if (*vlist_head == NULL)
		return 0 ;

	/* Determine which hash table this list uses */
	VarList **hash_head = (vlist_head == &__active_variable_head)
		? &__active_var_hash : &__var_list_hash;

	VarList *tmp1, *tmp2;
	tmp1 = *vlist_head;
	tmp2 = *vlist_tail;
	while((tmp2 != tmp1) && (tmp2->var.level == func_level)){
		VarList *tmp;
		tmp = tmp2;
		tmp2= tmp2->prev;
		tmp2->next = NULL;
		*vlist_tail = tmp2;
		if(tmp->var.ispointer)
			remove_heap_variables(&__var_heap_list_head,
								&__var_heap_list_tail,
								tmp->var.addr);
		HASH_DELETE(hh, *hash_head, tmp);
		free(tmp);
	}
	if ((tmp1 == tmp2) && (tmp1->var.level == func_level)){
		*vlist_head = NULL;
		*vlist_tail = NULL;
		tmp2 = NULL;
		if(tmp1->var.ispointer)
			remove_heap_variables(&__var_heap_list_head,
								&__var_heap_list_tail,
								tmp1->var.addr);
		HASH_DELETE(hh, *hash_head, tmp1);
		free(tmp1);
		return 0;
	}
	return 1;
}
/*
 * Generate variables list and their parallel level for parallel windows
 * Read variables form active variables list, group each variables is 4 bytes
 * If (list !EXISTS) create_new
 * else
 * 		copy_variable_with_new_level_of_parallel_region
 */
int generate_variable_list_for_parallel_windows(VarList *active_variable_head){
	unsigned int size, nelements;
	unsigned long addr;
	if (__var_list_head == NULL){
		if (active_variable_head == NULL) return 0;
		VarList *tmp;
		tmp = active_variable_head;
		while(tmp!=NULL){
			Var var;		
			var.dtype = tmp->var.dtype;
			var.level = __parallel_level__;
			var.ispointer = tmp->var.ispointer;
			var.pro = tmp->var.pro;	

			///TODO: COPY and ADD variables in to new list (note group of 4 bytes)			
			if(tmp->var.size < CAPE_WORD){
				size = CAPE_WORD;
				addr = tmp->var.addr & ~(CAPE_WORD - 1);
				if (tmp->var.n % CAPE_WORD==0) 
					nelements = tmp->var.n / CAPE_WORD;
				else
					nelements = tmp->var.n / CAPE_WORD + 1;
			}else {		
				size = tmp->var.size;
				nelements = tmp->var.n;
				addr = tmp->var.addr ;
			}
			var.n = nelements ;
			var.addr = addr ;
			var.size = size;			
			//add var into new list
			add_shared_variable(&__var_list_head, &__var_list_tail, var);	
	
			tmp = tmp->next;
		}
	}
	else{
		/* Nested parallel region: copy parent-level variables to the new level.
		 * __parallel_level__ was already incremented by open_parallel_window()
		 * before calling this function, so the parent level is __parallel_level__ - 1. */
		add_parallel_region(&__var_list_head, &__var_list_tail, __parallel_level__);
		return 0;
	}	
}

/*
 * add shared variable in to __var_list_head
 */
int add_shared_variable(VarList **vlist, VarList **vlist_tail, Var var){
	VarList *vl;
	if (var.addr <= 0) return 0;

	/* O(1) duplicate check via hash table */
	VarHashKey key;
	memset(&key, 0, sizeof(key));
	key.addr = var.addr;
	key.level = var.level;
	VarList *existing = NULL;
	HASH_FIND(hh, __var_list_hash, &key, sizeof(VarHashKey), existing);
	if (existing != NULL) return 2;

	vl = malloc(sizeof(struct VarList));
	memset(vl, 0, sizeof(struct VarList));
	vl->next = NULL;
	vl->prev = NULL;
	vl->var.addr = var.addr ;
	vl->var.size = var.size ;
	vl->var.n = var.n;
	vl->var.pro = var.pro;
	vl->var.level=var.level;
	vl->var.dtype =var.dtype;
	vl->var.ispointer = var.ispointer ;
	vl->hash_key.addr = var.addr;
	vl->hash_key.level = var.level;

	/* Add to hash table */
	HASH_ADD(hh, __var_list_hash, hash_key, sizeof(VarHashKey), vl);

	if (*vlist == NULL ) {
		*vlist = vl;
		*vlist_tail = vl;
		return 1;
	}

	VarList *tmp,*tmp2;
	tmp = *vlist;
	tmp2 = *vlist_tail;
	//Insert at the begin of list
	if (tmp->var.addr > vl->var.addr) {
		tmp->prev = vl;
		vl->next = tmp;
		*vlist = vl ;
		return 1;
	}
	//Insert at the end of list
	if(tmp2->var.addr < vl->var.addr){
		tmp2->next = vl;
		vl->prev = tmp2;
		*vlist_tail = vl;
		return 1;
	}

	//Find the position to insert
	while ((tmp->next != NULL) && (tmp->var.addr < vl->var.addr )){
		tmp = tmp->next;
	}
	//Insert before tmp
	if (tmp->var.addr > vl->var.addr){
		vl->next = tmp;
		vl->prev = tmp->prev;
		tmp->prev->next = vl;
		tmp->prev = vl ;
		return 1;
	}
	/* Should not reach here since we checked duplicates above */
	return 2;
} 

/*
 * --------------------------------------------------------------------
 * Add new pointer into heap list
 * 	manager_addr is not exists in heaplist
 * 	[addr, addr+len] is not exists in heaplist
 * --------------------------------------------------------------------
 */
int addnew_pointer(PointerList **hlist_head,
		PointerList **hlist_tail,
		Pointer pt){

	PointerList *item;
	item = malloc(sizeof(PointerList));
	memset(item, 0, sizeof(PointerList));
	item->pointer.manager_addr = pt.manager_addr;
	item->pointer.addr = pt.addr;
	item->pointer.len = pt.len ;
	item->next = NULL;
	item->prev = NULL;

	/* Add to hash table by manager_addr */
	HASH_ADD(hh_mgr, __heap_hash_mgr, pointer.manager_addr, sizeof(unsigned long), item);

	if (*hlist_head == NULL){
		*hlist_head = item ;
		*hlist_tail = item ;
		return 1 ;
	}

	PointerList *tmp_head, *tmp_tail;
	tmp_head = *hlist_head;
	tmp_tail = *hlist_tail;
	//insert at the begin of list
	if (tmp_head->pointer.addr < item->pointer.addr){
		item->next = tmp_head;
		tmp_head->prev = item;
		*hlist_head = item;
		return 1;
	}
	//insert at the end of list
	if (tmp_tail->pointer.addr > item->pointer.addr){
		tmp_tail->next = item ;
		item->prev = tmp_tail ;
		*hlist_tail = item ;
		return 1;
	}

	//find location to insert
	while((tmp_head !=NULL) && (tmp_head->pointer.addr < item->pointer.addr))
		tmp_head = tmp_head->next;

	//insert before tmp_head
	item->next = tmp_head;
	item->prev = tmp_head->prev;
	tmp_head->prev->next = item ;
	tmp_head->prev = item;
	return 1;
}
/*
 * --------------------------------------------------------------------
 * Remove head variables
 * Parameters:
 * 	manager address
 * Return
 * 	Head list pointers, that remove the item managered by manager_addr
 * -------------------------------------------------------------------
 */

int remove_heap_variables(PointerList **hlist_head,
		PointerList **hlist_tail,
		unsigned long manager_addr){

	if (*hlist_head == NULL) return 0;

	/* O(1) lookup via hash table */
	PointerList *tmp = NULL;
	HASH_FIND(hh_mgr, __heap_hash_mgr, &manager_addr, sizeof(unsigned long), tmp);
	if (tmp == NULL) return 0;

	/* Remove from hash table */
	HASH_DELETE(hh_mgr, __heap_hash_mgr, tmp);

	/* Remove from linked list */
	if (tmp == *hlist_head && tmp == *hlist_tail) {
		*hlist_head = NULL;
		*hlist_tail = NULL;
		free(tmp);
		return 1;
	}
	if (tmp == *hlist_head) {
		*hlist_head = tmp->next;
		tmp->next->prev = NULL;
		free(tmp);
		return 1;
	}
	if (tmp == *hlist_tail) {
		*hlist_tail = tmp->prev;
		tmp->prev->next = NULL;
		free(tmp);
		return 1;
	}
	tmp->prev->next = tmp->next;
	tmp->next->prev = tmp->prev;
	free(tmp);
	return 2;
}

/*
 * ---------------------------------------------------------------------
 * Remove item contain manager_addr or addr in heaplist
 * ---------------------------------------------------------------------
 */
/* Helper: unlink a PointerList node from linked list and hash, then free */
static void remove_pointer_node(PointerList **hlist_head, PointerList **hlist_tail, PointerList *node) {
	HASH_DELETE(hh_mgr, __heap_hash_mgr, node);
	if (node == *hlist_head && node == *hlist_tail) {
		*hlist_head = NULL;
		*hlist_tail = NULL;
	} else if (node == *hlist_head) {
		*hlist_head = node->next;
		node->next->prev = NULL;
	} else if (node == *hlist_tail) {
		*hlist_tail = node->prev;
		node->prev->next = NULL;
	} else {
		node->prev->next = node->next;
		node->next->prev = node->prev;
	}
	free(node);
}

int remove_exists_item(PointerList **hlist_head,
		PointerList **hlist_tail,
		Pointer pt){
	if (*hlist_head == NULL) return 0;

	PointerList *cur, *next_node;
	cur = *hlist_head;
	while (cur != NULL) {
		next_node = cur->next;
		if ((cur->pointer.manager_addr == pt.manager_addr) ||
				((cur->pointer.addr >= pt.addr)
						&& (cur->pointer.addr <= pt.addr + pt.len) ) ||
				((cur->pointer.addr + cur->pointer.len >= pt.addr)
						&& (cur->pointer.addr + cur->pointer.len <= pt.addr + pt.len) )
			){
			remove_pointer_node(hlist_head, hlist_tail, cur);
		}
		cur = next_node;
	}
	return 1 ;
}


/*
 * ---------------------------------------------------------------------
 * Open the window of variables
 * ---------------------------------------------------------------------
 */
void open_parallel_window(){	
	__is_inside_parallel_region__ = TRUE; // Enter parallel reion	
	__parallel_level__ ++;
	generate_variable_list_for_parallel_windows(__active_variable_head);	
}

/*
 * ---------------------------------------------------------------------
 * Close the window of variables
 * Release checkpoint memory
 * ---------------------------------------------------------------------
 */
void close_parallel_window(){

	if (__parallel_level__ > 0) {
		remove_active_variables(&__var_list_head, &__var_list_tail, __parallel_level__);
		__parallel_level__--;
	}
	if (__parallel_level__ == 0)
		__is_inside_parallel_region__ = FALSE; //Exit parallel region only at outermost level

}

/*
 * ---------------------------------------------------------------------
 * Add new region in parallel region
 * => copy variables of parent's level
 * ---------------------------------------------------------------------
 */
int add_parallel_region(VarList **vlist_head, VarList **vlist_tail, unsigned char level){
	
	VarList *vl_tail_level = NULL;
	VarList *vl_head = NULL;
	
	vl_head = *vlist_head;
	vl_tail_level = *vlist_tail;
	
	if(vl_head == NULL) return 0;
	VarList *copy_item = NULL;		
	VarList *item = NULL;
	
	item = vl_head;
	while(item != NULL && item->var.level != (level - 1))
		item = item->next;

	if (item == NULL) return 0; /* no parent-level variables found */

	//Copy items and assigned new level
	while(item != NULL && item->var.level == (level -1)){
		copy_item = malloc(sizeof(VarList));
		memset(copy_item, 0, sizeof(VarList));
		copy_item->var.addr = item->var.addr;
		copy_item->var.size = item->var.size;
		copy_item->var.n = item->var.n;
		copy_item->var.dtype = item->var.dtype;
		copy_item->var.pro = item->var.pro;
		copy_item->var.level = level; //New level
		copy_item->var.ispointer = item->var.ispointer;
		copy_item->hash_key.addr = item->var.addr;
		copy_item->hash_key.level = level;
		copy_item->next = NULL;
		copy_item->prev = vl_tail_level;
		if (vl_tail_level != NULL)
			vl_tail_level->next = copy_item;
		vl_tail_level = copy_item;
		HASH_ADD(hh, __var_list_hash, hash_key, sizeof(VarHashKey), copy_item);
		item = item->next;
	}
	*vlist_tail = vl_tail_level;
	return 1;
}
/*
 * ---------------------------------------------------------------------
 * Remove variables level
 *----------------------------------------------------------------------
 */
int remove_parellel_region(unsigned char level){
	if( level > 1){
		remove_active_variables(&__var_list_head, &__var_list_tail, level ); 
	}
	return 1;
}
/*
 * ---------------------------------------------------------------------
 * Find variable in VarList with address addr * 
 * ---------------------------------------------------------------------
 */
VarList * find_variable_by_addr(VarList *vlist, long addr, char level){
	/* O(1) lookup via hash table */
	VarHashKey key;
	memset(&key, 0, sizeof(key));
	key.addr = (unsigned long)addr;
	key.level = (unsigned char)level;

	/* Determine which hash table to search based on the list head */
	VarList *result = NULL;
	if (vlist == __active_variable_head) {
		HASH_FIND(hh, __active_var_hash, &key, sizeof(VarHashKey), result);
	} else {
		HASH_FIND(hh, __var_list_hash, &key, sizeof(VarHashKey), result);
	}
	return result;
}

/*
 * ---------------------------------------------------------------------
 * Generate checkpoint 
 * 	Checkpoint will be generated depending on properties of variables 
 * 	and the phase of execution model.
 * Checkpoint Structure:
 * C = {t , program counter, size_of_S, S, L}, 
 * 		where S = All modified data (addr, len, data)
 * 			  L = data of reduction variables (addr, len, data)
 *----------------------------------------------------------------------
 */
FILE *generate_checkpoint(VarList *vlist,	
			unsigned char level,
			unsigned char cflag,
			unsigned char ops_flag,
			unsigned long tsp,
			unsigned long pc ){
	
	VarList *v = NULL, *v1 = NULL;	
	unsigned long timestamp, programcounter;
	unsigned long size_s = 0;
	unsigned long start_addr;
	unsigned long  v_end, v_addr;
	unsigned long start;
	unsigned int len;
	unsigned long file_pointer = 0;
	char *data;
	
		
	FILE *stream;
	
	timestamp = tsp;
	/* Start with a small buffer; doubling realloc grows it to the
	 * actual delta size (a few MB), not the snapshot size (64 MB+).
	 * Pre-reserving to snapshot size + power-of-2 doubling was
	 * holding 128 MB per task — OOMed at every node count. */
	after_ckpt_release();
	after_ckpt_reserve(64 * 1024);
	stream = NULL;
	
	//write time stamp into checkpoint file
	after_ckpt_append(&timestamp, sizeof(unsigned long));
	
	//write program counter into checkpoint file
	programcounter = pc ;
	after_ckpt_append(&programcounter, sizeof(unsigned long));
	
	if (ckpt_data_size <= 0){ 
		size_s = sizeof(unsigned long) * 3;
		after_ckpt_append(&size_s, sizeof(unsigned long));
		return NULL;
	}
	
	//Write size_s into checkpoint file, and this place will be modified after writting S part
	after_ckpt_append(&size_s, sizeof(unsigned long));
	
	//Move to current window (the current level of VarList)
	v = vlist;
	while ((v != NULL) && (v->var.level != level))
		v = v->next;
		
	/* Bitmap-encoded S section. For each variable: scan word-by-word
	 * against the snapshot (ckpt_data) to build a per-page word bitmap,
	 * mark the page bitmap if any word changed, and emit only the
	 * dirty pages full-size. Page-aligned by construction. */
	unsigned int data_pointer;
	while (file_pointer < ckpt_data_size) {
		v_addr = (*(unsigned long *)(ckpt_data + file_pointer));
		v1 = find_variable_by_addr(v, v_addr, level);
		file_pointer += sizeof(unsigned long);
		data_pointer = file_pointer;
		file_pointer += v1->var.n * v1->var.size;

		if ((v1->var.pro == CAPE_PRIVATE) || (v1->var.pro == CAPE_THREAD_PRIVATE))
			continue;
		if ((cflag == ENTRY_CHECKPOINT) && (v1->var.pro == CAPE_LAST_PRIVATE))
			continue;
		if ((cflag == EXIT_CHECKPOINT) &&
		    ((v1->var.pro == CAPE_FIRST_PRIVATE) ||
		     (v1->var.pro == CAPE_COPY_IN) ||
		     (v1->var.pro == CAPE_SUM) || (v1->var.pro == CAPE_MUL) ||
		     (v1->var.pro == CAPE_MAX) || (v1->var.pro == CAPE_MIN)))
			continue;

		size_t var_bytes = (size_t)v1->var.n * (size_t)v1->var.size;
		size_t n_pages   = bmp_round_pages(var_bytes);
		size_t page_bmp_bytes = (n_pages + 7u) / 8u;

		/* Pre-reserve enough space for header + page bitmap. We
		 * append per-page payloads as we discover dirty pages, so
		 * the entry has variable size — fine, bb grows by 2x. */
		size_t hdr_off = after_ckpt_size;
		unsigned long var_addr_w = v_addr;
		unsigned long var_size_w = (unsigned long)var_bytes;
		uint32_t n_pages_w = (uint32_t)n_pages;
		uint32_t n_dirty_pages_w = 0;   /* patched at the end */
		after_ckpt_append(&var_addr_w,    sizeof(var_addr_w));
		after_ckpt_append(&var_size_w,    sizeof(var_size_w));
		after_ckpt_append(&n_pages_w,     sizeof(n_pages_w));
		size_t n_dirty_off = after_ckpt_size;
		after_ckpt_append(&n_dirty_pages_w, sizeof(n_dirty_pages_w));
		size_t page_bmp_off = after_ckpt_size;
		after_ckpt_reserve(after_ckpt_size + page_bmp_bytes);
		memset(after_ckpt + page_bmp_off, 0, page_bmp_bytes);
		after_ckpt_size += page_bmp_bytes;

		size_t n_dirty = 0;
		const unsigned char *snapshot_base =
			(const unsigned char *)ckpt_data + data_pointer;
		const unsigned char *live_base =
			(const unsigned char *)(uintptr_t)v_addr;

		/* IMPORTANT: every after_ckpt_append may realloc the
		 * underlying buffer, so we must NOT cache page_bmp_ptr
		 * across appends. Track the offset and recompute the
		 * pointer fresh each time we need to set a bit. */
		for (size_t p = 0; p < n_pages; p++) {
			size_t page_off = p << BMP_PAGE_SHIFT;
			size_t page_len = BMP_PAGE_SIZE;
			if (page_off + page_len > var_bytes)
				page_len = var_bytes - page_off;

			unsigned char wbmp[BMP_WORD_BMP_BYTES];
			memset(wbmp, 0, sizeof(wbmp));
			int dirty = 0;
			const int *live  = (const int *)(live_base  + page_off);
			const int *snap  = (const int *)(snapshot_base + page_off);
			size_t nwords = page_len / CAPE_WORD;
			for (size_t w = 0; w < nwords; w++) {
				if (live[w] != snap[w]) {
					bmp_set(wbmp, w);
					dirty = 1;
				}
			}
			if (!dirty)
				continue;
			bmp_set((unsigned char *)after_ckpt + page_bmp_off, p);
			n_dirty++;
			/* Append [word_bmp][page_data] for this dirty page. */
			after_ckpt_append(wbmp, BMP_WORD_BMP_BYTES);
			after_ckpt_reserve(after_ckpt_size + BMP_PAGE_SIZE);
			memcpy(after_ckpt + after_ckpt_size, live_base + page_off, page_len);
			if (page_len < BMP_PAGE_SIZE)
				memset(after_ckpt + after_ckpt_size + page_len, 0,
				       BMP_PAGE_SIZE - page_len);
			after_ckpt_size += BMP_PAGE_SIZE;
		}
		n_dirty_pages_w = (uint32_t)n_dirty;
		after_ckpt_patch(n_dirty_off, &n_dirty_pages_w,
		                 sizeof(n_dirty_pages_w));
		(void)hdr_off;
	}
	//Identity size of S part
	size_s = after_ckpt_size;		
	//Write L part into checkpoint
	if((cflag == EXIT_CHECKPOINT) && (ops_flag==TRUE)){
		//Move to current window (the current level of VarList)
		v = vlist;
		while ((v != NULL) && (v->var.level != level))
			v = v->next;
		while ((v != NULL) && (v->var.level == level)){
			if ((v->var.pro == CAPE_SUM) || (v->var.pro == CAPE_MUL) || (v->var.pro == CAPE_MAX) ||(v->var.pro == CAPE_MIN) ){
				len = v->var.size * v->var.n;
				after_ckpt_append(&v->var.addr, sizeof(long));
				after_ckpt_append(&len, sizeof(unsigned int));
				after_ckpt_append((void*)(uintptr_t)v->var.addr, len);
			}
			v = v->next;
		}			
	}
	after_ckpt_patch(2 * sizeof(unsigned long), &size_s, sizeof(unsigned long));
	
	return NULL;

}

/**
 * ---------------------------------------------------------------------
 * Merge S = S1 + S2
 * Write S into after_checkpoint
 * Structer of S part is: {(addr, len, data) .... }
 * --------------------------------------------------------------------- 
 */
/* Bitmap-format merge: parse var entries from s1 and s2 in lockstep
 * (CAPE invariant: same VarList order on every rank), and for each var
 * compute page_bmp_out = page_bmp_A | page_bmp_B. Pages that are dirty
 * on only one side are bulk-copied verbatim (no per-word work). Pages
 * dirty on both sides have their word bitmaps OR'd and the data merged
 * word-by-word, with B winning on collisions (B is the "newer" side
 * picked by the caller's timestamp comparison). */
static int merge_data_bitmap_one(const char *s1, size_t e1, size_t *p1_io,
				 const char *s2, size_t e2, size_t *p2_io)
{
	size_t p1 = *p1_io;
	size_t p2 = *p2_io;

	if ((e1 - p1) < 24 || (e2 - p2) < 24)
		return -1;

	unsigned long va1 = *(const unsigned long *)(s1 + p1); p1 += 8;
	unsigned long vs1 = *(const unsigned long *)(s1 + p1); p1 += 8;
	uint32_t np1 = *(const uint32_t *)(s1 + p1); p1 += 4;
	uint32_t nd1 = *(const uint32_t *)(s1 + p1); p1 += 4;

	unsigned long va2 = *(const unsigned long *)(s2 + p2); p2 += 8;
	unsigned long vs2 = *(const unsigned long *)(s2 + p2); p2 += 8;
	uint32_t np2 = *(const uint32_t *)(s2 + p2); p2 += 4;
	uint32_t nd2 = *(const uint32_t *)(s2 + p2); p2 += 4;

	if (va1 != va2 || vs1 != vs2 || np1 != np2) {
		fprintf(stderr,
		        "CAPE merge_data: variable mismatch addr1=0x%lx addr2=0x%lx size1=%lu size2=%lu np1=%u np2=%u\n",
		        va1, va2, vs1, vs2, np1, np2);
		return -1;
	}

	uint32_t n_pages = np1;
	size_t page_bmp_bytes = ((size_t)n_pages + 7u) / 8u;

	const unsigned char *pbmp1 = (const unsigned char *)(s1 + p1);
	p1 += page_bmp_bytes;
	const unsigned char *entries1 = (const unsigned char *)(s1 + p1);
	p1 += (size_t)nd1 * BMP_PAGE_ENTRY_BYTES;

	const unsigned char *pbmp2 = (const unsigned char *)(s2 + p2);
	p2 += page_bmp_bytes;
	const unsigned char *entries2 = (const unsigned char *)(s2 + p2);
	p2 += (size_t)nd2 * BMP_PAGE_ENTRY_BYTES;

	/* Append the merged var entry directly into after_ckpt. */
	size_t hdr_off = after_ckpt_size;
	after_ckpt_append(&va1, sizeof(va1));
	after_ckpt_append(&vs1, sizeof(vs1));
	after_ckpt_append(&n_pages, sizeof(n_pages));
	uint32_t merged_n_dirty = 0;
	size_t nd_off = after_ckpt_size;
	after_ckpt_append(&merged_n_dirty, sizeof(merged_n_dirty));

	/* page_bmp_out = pbmp1 | pbmp2  (single linear OR pass, fast). */
	size_t out_pbmp_off = after_ckpt_size;
	after_ckpt_reserve(after_ckpt_size + page_bmp_bytes);
	unsigned char *out_pbmp = (unsigned char *)after_ckpt + out_pbmp_off;
	for (size_t i = 0; i < page_bmp_bytes; i++)
		out_pbmp[i] = pbmp1[i] | pbmp2[i];
	after_ckpt_size += page_bmp_bytes;

	/* Walk pages: for each set bit in the OR'd page bitmap, emit one
	 * BMP_PAGE_ENTRY_BYTES record. Per-side cursors track which entry
	 * to consume next from each side. */
	size_t i1 = 0, i2 = 0;
	for (uint32_t p = 0; p < n_pages; p++) {
		int in1 = bmp_get(pbmp1, p);
		int in2 = bmp_get(pbmp2, p);
		if (!in1 && !in2)
			continue;
		merged_n_dirty++;
		if (in1 && !in2) {
			after_ckpt_append(entries1 + i1 * BMP_PAGE_ENTRY_BYTES,
			                  BMP_PAGE_ENTRY_BYTES);
			i1++;
		} else if (!in1 && in2) {
			after_ckpt_append(entries2 + i2 * BMP_PAGE_ENTRY_BYTES,
			                  BMP_PAGE_ENTRY_BYTES);
			i2++;
		} else {
			const unsigned char *ea = entries1 + i1 * BMP_PAGE_ENTRY_BYTES;
			const unsigned char *eb = entries2 + i2 * BMP_PAGE_ENTRY_BYTES;
			unsigned char merged[BMP_PAGE_ENTRY_BYTES];
			/* OR the word bitmaps. */
			for (size_t b = 0; b < BMP_WORD_BMP_BYTES; b++)
				merged[b] = ea[b] | eb[b];
			/* For each word, pick: A_only -> A, B_only -> B,
			 * both -> B (newer wins). When neither set, copy
			 * either (they should match — both are unmodified). */
			const int *da = (const int *)(ea + BMP_WORD_BMP_BYTES);
			const int *db = (const int *)(eb + BMP_WORD_BMP_BYTES);
			int *dout = (int *)(merged + BMP_WORD_BMP_BYTES);
			for (size_t w = 0; w < BMP_WORDS_PER_PAGE; w++) {
				int aset = bmp_get(ea, w);
				int bset = bmp_get(eb, w);
				if (bset)
					dout[w] = db[w];
				else if (aset)
					dout[w] = da[w];
				else
					dout[w] = da[w];
			}
			after_ckpt_append(merged, BMP_PAGE_ENTRY_BYTES);
			i1++;
			i2++;
		}
	}

	after_ckpt_patch(nd_off, &merged_n_dirty, sizeof(merged_n_dirty));
	(void)hdr_off;
	*p1_io = p1;
	*p2_io = p2;
	return 0;
}

int merge_data(char *s1, unsigned int pos_s1, unsigned size_s1,
				char *s2, unsigned int pos_s2, unsigned size_s2){
	size_t p1 = pos_s1;
	size_t p2 = pos_s2;
	size_t end_s1 = (size_t)pos_s1 + size_s1;
	size_t end_s2 = (size_t)pos_s2 + size_s2;

	if (p1 >= end_s1 && p2 >= end_s2)
		return 0;
	if (p1 >= end_s1) {
		after_ckpt_append(s2 + p2, end_s2 - p2);
		return 1;
	}
	if (p2 >= end_s2) {
		after_ckpt_append(s1 + p1, end_s1 - p1);
		return 1;
	}

	/* Lockstep merge of var entries (same order on both sides). */
	while (p1 < end_s1 && p2 < end_s2) {
		if (merge_data_bitmap_one(s1, end_s1, &p1, s2, end_s2, &p2) < 0)
			return -1;
	}

	/* Pass through any leftover var entries (shouldn't happen for
	 * matching VarLists, but handle defensively). */
	if (p1 < end_s1)
		after_ckpt_append(s1 + p1, end_s1 - p1);
	if (p2 < end_s2)
		after_ckpt_append(s2 + p2, end_s2 - p2);

	return 2;
}

#if 0  /* kept dead code below for reference; bitmap path replaces it */
int merge_data_old(char *s1, unsigned int pos_s1, unsigned size_s1,
				char *s2, unsigned int pos_s2, unsigned size_s2){
	unsigned int p1, p2;
	unsigned int end_s1, end_s2;
	unsigned int len, len1, len2;
	long addr1, addr2, old_addr2;
	p1 = pos_s1;
	p2 = pos_s2;
	end_s1 = pos_s1 + size_s1;
	end_s2 = pos_s2 + size_s2;
	while ((p1 < end_s1) && (p2 < end_s2)){
		//printf("\n Node %ld: (0x%lx - %ld ) + (0x%lx - %ld)", __node__, addr1, len1, addr2, len2) ;
		if (addr1 <= addr2){
			/* The source already stores [addr:8][len:4][data] in
			 * one contiguous chunk at s1 + p1 - 12, so a single
			 * fwrite of 12 + len bytes replaces three. Cuts the
			 * stdio call count (and lock churn) by ~3x — the
			 * dominant cost at scale. */
			if (len1 > (end_s1 - p1))
				return -1;
			after_ckpt_append(s1 + p1 - (sizeof(long) + sizeof(unsigned int)), sizeof(long) + sizeof(unsigned int) + len1);
			p1 += len1;
			//printf("\n Node %ld: Write: 0x%lx  : %ld ", __node__, addr1, len1) ;
			//S1: [-----------)  
			//S2:     -----		 ------	
			if ((addr1 + len1) >= (addr2+ len2)){
				p2 += len2;
				if (p2 < end_s2) {
					if ((end_s2 - p2) < (sizeof(long) + sizeof(unsigned int)))
						return -1;
					addr2 = *(long *) (s2 + p2 );
					p2 += sizeof(long);
					len2 = *(unsigned int *) (s2 + p2) ;
					p2 += sizeof(unsigned int);
					if (len2 > (end_s2 - p2))
						return -1;
				}
			}			
			//S1: [-----------)       [--------)
			//S2:        -----[----)
			if (((addr1 + len1) >= addr2) && ((addr1 + len1) < (addr2 + len2))){
				old_addr2 = addr2; //save the old addr
				addr2 = addr1 + len1;
				len2 = len2 - (addr2 - old_addr2);
				p2 += (addr2 - old_addr2);			
			}			
			if (p1 < end_s1) {
				if ((end_s1 - p1) < (sizeof(long) + sizeof(unsigned int)))
					return -1;
				addr1 = *(long *) (s1 + p1 );
				p1 += sizeof(long);
				len1 = *(unsigned int *) (s1 + p1) ;
				p1 += sizeof(unsigned int);
				if (len1 > (end_s1 - p1))
					return -1;
			}					
		}else{ //addr1 > addr2
			//S1:                      [-----------)         [--------)
			//S2:        [--------)		    -------[------)	
			if ((addr2 + len2) < addr1) {
				if (len2 > (end_s2 - p2))
					return -1;
				after_ckpt_append(s2 + p2 - (sizeof(long) + sizeof(unsigned int)), sizeof(long) + sizeof(unsigned int) + len2);
				//printf("\n Node %ld: Write: 0x%lx : %ld ", __node__, addr2, len2) ;
				p2 += len2;					
				if (p2 >= end_s2) break;
				if ((end_s2 - p2) < (sizeof(long) + sizeof(unsigned int)))
					return -1;
				addr2 = *(long *) (s2 + p2 );
				p2 += sizeof(long);
				len2 = *(unsigned int *) (s2 + p2) ;
				p2 += sizeof(unsigned int);												
				if (len2 > (end_s2 - p2))
					return -1;
			//S1:       [-----------)        [--------)
			//S2:       	 -------[--------)--------[------)	
			}else {
				/* Partial entry: header has computed len, not
				 * the original len2. Pack header on stack and
				 * issue 2 writes instead of 3. */
				unsigned char _hdr[sizeof(long) + sizeof(unsigned int)];
				len = addr1 - addr2;
				memcpy(_hdr, &addr2, sizeof(long));
				memcpy(_hdr + sizeof(long), &len, sizeof(unsigned int));
				if (len > (end_s2 - p2))
					return -1;
				after_ckpt_append(_hdr, sizeof(_hdr));
				after_ckpt_append(s2 + p2, len);
				//printf("\n Node %ld: Write: 0x%lx : %ld ", __node__, addr2, len2) ;
				p2 += len;

				len2 = len2-len;	
				addr2 = addr1;							
			}
		}	
	}	
	/* Merge the rest part — once one side is exhausted, every remaining
	 * entry from the other side is passed through unchanged. s1 is
	 * never modified in-place, so we can bulk-copy [header + all
	 * subsequent entries] in one fwrite. s2 may carry a residual header
	 * (set by the partial-entry else branch above) that doesn't match
	 * what's in the source buffer, so write the current header from
	 * locals and bulk-copy only the unmodified tail. */
	if (p1 < end_s1){
		after_ckpt_append(s1 + p1 - (sizeof(long) + sizeof(unsigned int)), (end_s1 - p1) + (sizeof(long) + sizeof(unsigned int)));
		p1 = end_s1;
	}
	if (p2 < end_s2){
		unsigned char _hdr[sizeof(long) + sizeof(unsigned int)];
		if (len2 > (end_s2 - p2))
			return -1;
		memcpy(_hdr, &addr2, sizeof(long));
		memcpy(_hdr + sizeof(long), &len2, sizeof(unsigned int));
		after_ckpt_append(_hdr, sizeof(_hdr));
		after_ckpt_append(s2 + p2, len2);
		p2 += len2;
		if (p2 < end_s2) {
			after_ckpt_append(s2 + p2, end_s2 - p2);
			p2 = end_s2;
		}
	}
	/* Single flush at the end — open_memstream's size is only updated
	 * on fflush/fclose, and the caller reads after_ckpt_size right
	 * after we return. The per-fwrite fflushes that used to be inside
	 * this function dominated merge_checkpoint at scale. */
	return 2;
}
#endif  /* dead old merge_data_old */

static int validate_checkpoint_s_section(const char *ckpt,
		size_t total_size,
		unsigned long size_s,
		const char *label)
{
	const unsigned long ckpt_header_size = sizeof(unsigned long) * 3;
	unsigned long p = ckpt_header_size;

	if (total_size < ckpt_header_size) {
		fprintf(stderr, "CAPE %s: checkpoint too small (%zu)\n", label, total_size);
		return -1;
	}
	if ((size_s < ckpt_header_size) || (size_s > total_size)) {
		fprintf(stderr, "CAPE %s: invalid size_s=%lu total=%zu\n",
		        label, size_s, total_size);
		return -1;
	}

	/* Bitmap format: each var is [addr:8][size:8][n_pages:4][n_dirty:4]
	 * [page_bmp][n_dirty * (word_bmp + page_data)]. */
	while (p < size_s) {
		if ((size_s - p) < 24) {
			fprintf(stderr, "CAPE %s: truncated bitmap header at %lu/%lu\n",
			        label, p, size_s);
			return -1;
		}
		p += sizeof(unsigned long);   /* var_addr */
		p += sizeof(unsigned long);   /* var_size */
		uint32_t n_pages   = *(uint32_t *)(ckpt + p); p += 4;
		uint32_t n_dirty   = *(uint32_t *)(ckpt + p); p += 4;
		size_t page_bmp_bytes = ((size_t)n_pages + 7u) / 8u;
		size_t entries_bytes  = (size_t)n_dirty * BMP_PAGE_ENTRY_BYTES;
		if ((size_s - p) < page_bmp_bytes + entries_bytes) {
			fprintf(stderr, "CAPE %s: truncated bitmap entry at %lu/%lu\n",
			        label, p, size_s);
			return -1;
		}
		p += page_bmp_bytes;
		p += entries_bytes;
	}
	return 0;
}
/*
 * ---------------------------------------------------------------------
 * Merge a buffer to after_checkpoint
 * 	if (t1 >= t2) 
 * 		{ C <- t1 , pc1, size_s, S1 + S2}
 * 	else
 * 		{ C <- t2, pc2, size_s, S2+S1}
 * --------------------------------------------------------------------- 
 */
int merge_checkpoint(char *src_ckpt, size_t src_size, char ckpt_flag){	
	
	FILE *tmp_stream = NULL;
	char *tmp_ckpt = NULL;
	size_t tmp_size = 0;
	const unsigned long ckpt_header_size = sizeof(unsigned long) * 3;
	
	unsigned int src_pointer =0, tmp_pointer =0;
	
	if (src_size < ckpt_header_size)
		return 0;
	
	if (after_ckpt_size == 0){
		unsigned long size_s_src = *(unsigned long *)(src_ckpt + (2 * sizeof(unsigned long)));
		if (validate_checkpoint_s_section(src_ckpt, src_size, size_s_src, "merge_checkpoint[src_init]") < 0)
			return -1;
		/* Manual buffer: skip stdio entirely. */
		after_ckpt_release();
		after_ckpt_reserve(src_size);
		memcpy(after_ckpt, src_ckpt, src_size);
		after_ckpt_size = src_size;
		return src_size;
	}
	/* Transfer ownership of after_ckpt into tmp_ckpt (no memcpy), and
	 * pre-allocate a fresh after_ckpt sized to the worst-case merged
	 * output. With a known capacity, every subsequent append into
	 * after_ckpt is pure memcpy — no realloc, no stdio cookie_write. */
	if (after_ckpt_stream != NULL) {
		if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
		after_ckpt_stream = NULL;
	}
	tmp_ckpt = after_ckpt;
	tmp_size = after_ckpt_size;
	tmp_stream = NULL;
	after_ckpt = NULL;
	after_ckpt_size = 0;
	after_ckpt_cap = 0;
	/* Exact allocation here: tmp_size + src_size is a tight upper
	 * bound on the merged output, so doubling-realloc would round up
	 * to 2x and waste tens of MB at later hypercube steps. */
	after_ckpt_reserve_exact(tmp_size + src_size + 64);
		
	src_pointer = 0;
 	tmp_pointer =0 ;
 	unsigned long t1, t2;
 	unsigned long pc1, pc2;
 	unsigned long size_s = 0, size_s1, size_s2;
	int merge_rc = 0;
  	
	
 	t1 = *(unsigned long *)tmp_ckpt;
 	t2 = *(unsigned long *)src_ckpt;  	 	
 	tmp_pointer += sizeof(unsigned long);
 	src_pointer += sizeof(unsigned long);
 	
 	pc1 =  *(unsigned long *)(tmp_ckpt + tmp_pointer);
 	pc2 =  *(unsigned long *)(src_ckpt + src_pointer);	 	
 	src_pointer += sizeof(unsigned long);
 	tmp_pointer += sizeof(unsigned long);
 	
 	size_s1 =  *(unsigned long *)(tmp_ckpt + tmp_pointer);
 	size_s2 =  *(unsigned long *)(src_ckpt + src_pointer);
	if ((size_s1 < ckpt_header_size) || (size_s1 > tmp_size) ||
	    (size_s2 < ckpt_header_size) || (size_s2 > src_size)) {
		fprintf(stderr,
		        "CAPE merge_checkpoint: invalid size_s values (size_s1=%lu tmp_size=%zu size_s2=%lu src_size=%zu)\n",
		        size_s1, tmp_size, size_s2, src_size);
		fprintf(stderr,
		        "CAPE merge_checkpoint: header dump local(t=%lu pc=0x%lx) remote(t=%lu pc=0x%lx)\n",
		        t1, pc1, t2, pc2);
		if (tmp_stream) fclose(tmp_stream);
		free(tmp_ckpt);
		return -1;
	}
	if (validate_checkpoint_s_section(tmp_ckpt, tmp_size, size_s1, "merge_checkpoint[tmp]") < 0 ||
	    validate_checkpoint_s_section(src_ckpt, src_size, size_s2, "merge_checkpoint[src]") < 0) {
		if (tmp_stream) fclose(tmp_stream);
		free(tmp_ckpt);
		return -1;
	}
 	 	
 	src_pointer += sizeof(unsigned long);
 	tmp_pointer += sizeof(unsigned long);
	 	
 	/* No stdio — after_ckpt was already pre-reserved above. */
 	after_ckpt_stream = NULL;

 	
 	if (t1 >= t2){ 			//C <- t1, pc1, size_s, S1+ S2		
		after_ckpt_append(&t1, sizeof(unsigned long));
		after_ckpt_append(&pc1, sizeof(unsigned long));
		after_ckpt_append(&size_s, sizeof(unsigned long));
		if ((size_s1 > tmp_pointer) && (size_s2 > src_pointer))
			merge_rc = merge_data(tmp_ckpt, tmp_pointer, size_s1 - tmp_pointer, src_ckpt, src_pointer, size_s2 - src_pointer);
		else if (size_s1 > tmp_pointer)
			after_ckpt_append(tmp_ckpt + tmp_pointer, size_s1 - tmp_pointer);
		else if (size_s2 > src_pointer)
			after_ckpt_append(src_ckpt + src_pointer, size_s2 - src_pointer);
	}else{	//C <-	t2, pc2, size_s, S2 + S1		
		after_ckpt_append(&t2, sizeof(unsigned long));
		after_ckpt_append(&pc2, sizeof(unsigned long));
		after_ckpt_append(&size_s, sizeof(unsigned long));
		if ((size_s1 > tmp_pointer) && (size_s2 > src_pointer))
			merge_rc = merge_data(src_ckpt, src_pointer, size_s2 - src_pointer, tmp_ckpt, tmp_pointer, size_s1 - tmp_pointer);
		else if (size_s2 > src_pointer)
			after_ckpt_append(src_ckpt + src_pointer, size_s2 - src_pointer);
		else if (size_s1 > tmp_pointer)
			after_ckpt_append(tmp_ckpt + tmp_pointer, size_s1 - tmp_pointer);
	}
	if (merge_rc < 0) {
		fprintf(stderr, "CAPE merge_checkpoint: malformed S section (size_s1=%lu size_s2=%lu)\n",
		        size_s1, size_s2);
		if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
		after_ckpt_stream = NULL;
		after_ckpt_release();
		if (tmp_stream) fclose(tmp_stream);
		free(tmp_ckpt);
		tmp_ckpt = NULL;
		return -1;
	}
	tmp_pointer = size_s1;
	src_pointer = size_s2;	
	size_s = after_ckpt_size;
	
	//Check if exist reduction data
	if (ckpt_flag == EXIT_CHECKPOINT){ 
		while (	(tmp_pointer < tmp_size) && (src_pointer <src_size)){		
			long addr, src_addr;
			unsigned int len, src_len;
			
			if ((tmp_size - tmp_pointer) < (sizeof(long) + sizeof(unsigned int)) ||
			    (src_size - src_pointer) < (sizeof(long) + sizeof(unsigned int))) {
				fprintf(stderr, "CAPE merge_checkpoint: malformed L header\n");
				if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
				after_ckpt_stream = NULL;
				after_ckpt_release();
				if (tmp_stream) fclose(tmp_stream);
				free(tmp_ckpt);
				return -1;
			}

			addr = *((long*)(tmp_ckpt + tmp_pointer));
			src_addr = *((long*)(src_ckpt + src_pointer));
			tmp_pointer += sizeof(long);
			src_pointer += sizeof(long);

			len = *(unsigned int *) (tmp_ckpt + tmp_pointer) ;
			src_len = *(unsigned int *) (src_ckpt + src_pointer) ;
			tmp_pointer += sizeof(unsigned int);
			src_pointer += sizeof(unsigned int);
			if ((addr != src_addr) || (len != src_len)) {
				fprintf(stderr,
				        "CAPE merge_checkpoint: L mismatch (addr=0x%lx src_addr=0x%lx len=%u src_len=%u)\n",
				        addr, src_addr, len, src_len);
				if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
				after_ckpt_stream = NULL;
				after_ckpt_release();
				if (tmp_stream) fclose(tmp_stream);
				free(tmp_ckpt);
				return -1;
			}
			if ((len > (tmp_size - tmp_pointer)) || (len > (src_size - src_pointer))) {
				fprintf(stderr,
				        "CAPE merge_checkpoint: malformed L payload (len=%u tmp_left=%zu src_left=%zu)\n",
				        len, tmp_size - tmp_pointer, src_size - src_pointer);
				if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
				after_ckpt_stream = NULL;
				after_ckpt_release();
				if (tmp_stream) fclose(tmp_stream);
				free(tmp_ckpt);
				return -1;
			}
			
			after_ckpt_append(&addr, sizeof(long));
			after_ckpt_append(&len, sizeof(unsigned int));

			VarList *var = NULL; 
			var = find_variable_by_addr(__var_list_head, addr , __parallel_level__);	
			
			if (var == NULL) {
				fprintf(stderr, "CAPE merge_checkpoint: variable not found for addr=0x%lx level=%d\n",
				        addr, __parallel_level__);
				if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
				after_ckpt_stream = NULL;
				after_ckpt_release();
				if (tmp_stream) fclose(tmp_stream);
				free(tmp_ckpt);
				return -1; //ERROR
			}

			int num, n1, n2;
			unsigned long num_l, n1_l, n2_l;
			float num_f, n1_f, n2_f;
			double num_d, n1_d, n2_d;			
			switch (var->var.dtype){
				case CAPE_CHAR:
				case CAPE_INT:
				case CAPE_LONG:					
					n1 =  *(long*) (tmp_ckpt + tmp_pointer);
					n2=  *(long*) (src_ckpt + src_pointer) ;
					if (var->var.pro == CAPE_SUM){
						num = 0;
						num = n1 + n2;
						after_ckpt_append(&num, len);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num = 1;
						num = n1 * n2;		
						after_ckpt_append(&num, len);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num = (n1 >= n2 )? n1 : n2 ;
						after_ckpt_append(&num, len);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
						num = (n1 < n2 )? n1 : n2 ;				
						after_ckpt_append(&num, len);
						break;						
					}							
					break;
				case CAPE_UNSIGNED_CHAR:	
				case CAPE_UNSIGNED_INT:
				case CAPE_UNSIGNED_LONG:					
					n1_l =  *(unsigned long*) (tmp_ckpt + tmp_pointer);
					n2_l=  *(unsigned long*) (src_ckpt + src_pointer) ;
					if (var->var.pro == CAPE_SUM){
						num_l = 0;
						num_l = n1_l + n2_l;
						after_ckpt_append(&num_l, len);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num_l = 1;
						num_l = n1_l * n2_l;		
						after_ckpt_append(&num_l, len);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num_l = (n1_l >= n2_l )? n1_l : n2_l ;
						after_ckpt_append(&num_l, len);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
						num_l = (n1_l < n2_l )? n1_l : n2_l ;				
						after_ckpt_append(&num_l, len);
						break;						
					}							
					break;
				case CAPE_FLOAT:					
					n1_f =  *(float*) (tmp_ckpt + tmp_pointer);
					n2_f=  *(float*) (src_ckpt + src_pointer) ;
					if (var->var.pro == CAPE_SUM){
						num_f = 0.0;
						num_f = n1_f + n2_f;
						after_ckpt_append(&num_f, len);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num_f = 1.0;
						num_f = n1_f * n2_f;		
						after_ckpt_append(&num_f, len);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num_f = (n1_f >= n2_f )? n1_f : n2_f ;
						after_ckpt_append(&num_f, len);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
						num_f = (n1_f < n2_f )? n1_f : n2_f ;				
						after_ckpt_append(&num_f, len);
						break;						
					}							
					break;
				case CAPE_DOUBLE:					
					n1_d =  *(double*) (tmp_ckpt + tmp_pointer);
					n2_d=  *(double*) (src_ckpt + src_pointer);					
					if (var->var.pro == CAPE_SUM){
						num_d = 0.0;
						num_d = n1_d + n2_d;
						after_ckpt_append(&num_d, len);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num_d = 1.0;
						num_d = n1_d * n2_d;		
						after_ckpt_append(&num_d, len);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num = (n1_d >= n2_d )? n1_d : n2_d ;
						after_ckpt_append(&num_d, len);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
						num_d = (n1_d < n2_d )? n1_d : n2_d ;				
						after_ckpt_append(&num_d, len);
						break;						
					}							
					break;
				default:
					printf("This datatype is not supported !!!!!");
					if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
					after_ckpt_stream = NULL;
					after_ckpt_release();
					if (tmp_stream) fclose(tmp_stream);
					free(tmp_ckpt);
					return -1;
			}
			
			src_pointer += len;
			tmp_pointer += len;
		}
		if ((tmp_pointer != tmp_size) || (src_pointer != src_size)) {
			fprintf(stderr,
			        "CAPE merge_checkpoint: uneven L sections (tmp_pointer=%u tmp_size=%zu src_pointer=%u src_size=%zu)\n",
			        tmp_pointer, tmp_size, src_pointer, src_size);
			if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
			after_ckpt_stream = NULL;
			after_ckpt_release();
			if (tmp_stream) fclose(tmp_stream);
			free(tmp_ckpt);
			return -1;
		}
		

	}	
	/* Patch size_s into the header at offset 2*ulong (replacing the
	 * placeholder written earlier). Direct memcpy — no seeks. */
	after_ckpt_patch(2 * sizeof(unsigned long), &size_s,
	                 sizeof(unsigned long));

	if (tmp_stream) fclose(tmp_stream);
	free(tmp_ckpt);	
	tmp_ckpt = NULL;
	
	return src_size;	
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

/* ====================================================================
 * Optional zlib compression for checkpoint exchange.
 * Compile with -DCAPE_COMPRESS and link with -lz to enable.
 * Only compresses when the result is actually smaller than the input.
 * ==================================================================== */
#ifdef CAPE_COMPRESS

/* Minimum payload size worth trying to compress (bytes) */
#define CAPE_COMPRESS_THRESHOLD 256

/*
 * Compress `src` of `src_len` bytes into a newly malloc'd buffer.
 * Returns compressed buffer in *out, compressed size in *out_len.
 * If compression doesn't shrink the data, returns NULL (caller should
 * send uncompressed).  The first 4 bytes of the returned buffer store
 * the original uncompressed length (uint32_t, little-endian) so the
 * receiver knows how much to allocate for decompression.
 */
static char *cape_compress(const char *src, size_t src_len, size_t *out_len)
{
	if (src_len < CAPE_COMPRESS_THRESHOLD) return NULL;

	uLongf bound = compressBound((uLong)src_len);
	/* Layout: [uint32_t orig_len][compressed bytes] */
	size_t buf_len = sizeof(uint32_t) + (size_t)bound;
	char *buf = malloc(buf_len);
	if (!buf) return NULL;

	uLongf comp_len = bound;
	int rc = compress2((Bytef *)(buf + sizeof(uint32_t)), &comp_len,
	                   (const Bytef *)src, (uLong)src_len, Z_DEFAULT_COMPRESSION);
	if (rc != Z_OK || (sizeof(uint32_t) + comp_len) >= src_len) {
		free(buf);
		return NULL; /* compression didn't help */
	}

	uint32_t orig = (uint32_t)src_len;
	memcpy(buf, &orig, sizeof(uint32_t));
	*out_len = sizeof(uint32_t) + (size_t)comp_len;
	return buf;
}

/*
 * Decompress a buffer produced by cape_compress.
 * Returns newly malloc'd buffer with the original data; sets *out_len.
 * Returns NULL on error.
 */
static char *cape_decompress(const char *src, size_t src_len, size_t *out_len)
{
	if (src_len < sizeof(uint32_t)) return NULL;

	uint32_t orig;
	memcpy(&orig, src, sizeof(uint32_t));

	char *buf = malloc((size_t)orig);
	if (!buf) return NULL;

	uLongf decomp_len = (uLongf)orig;
	int rc = uncompress((Bytef *)buf, &decomp_len,
	                    (const Bytef *)(src + sizeof(uint32_t)),
	                    (uLong)(src_len - sizeof(uint32_t)));
	if (rc != Z_OK) {
		free(buf);
		return NULL;
	}
	*out_len = (size_t)decomp_len;
	return buf;
}

#endif /* CAPE_COMPRESS */

/*
 *---------------------------------------------------------------------
 * Send and Receive checkpoint based on hypercube algorithm
 *----------------------------------------------------------------------
 */
int hypercube_allreduce(unsigned int node, unsigned int num_nodes, char ckpt_flag, uint32_t epoch){
	int i, nsteps = 0;
	unsigned int partner;
	unsigned long send_msg_size=0, recv_msg_size = 0;
	int rc = 0;
	uint32_t size_token, data_token;
	char *send_msg;
	char *recv_msg;

	nsteps = mylog2(num_nodes);

	send_msg = after_ckpt;
	send_msg_size = after_ckpt_size;

	for(i = 0; i < nsteps; i++){
		partner = node ^ (1 << i);
		size_token = ((epoch & 0x0fffU) << 8) | (uint32_t)((i * 2) & 0xff);
		data_token = ((epoch & 0x0fffU) << 8) | (uint32_t)(((i * 2) + 1) & 0xff);

#ifdef CAPE_COMPRESS
		/* Try to compress the outgoing message */
		char *comp_buf = NULL;
		size_t comp_len = 0;
		unsigned long send_hdr[2]; /* [0]=wire_size, [1]=orig_size (0=uncompressed) */
		unsigned long recv_hdr[2];

		comp_buf = cape_compress(send_msg, send_msg_size, &comp_len);
		if (comp_buf) {
			send_hdr[0] = (unsigned long)comp_len;
			send_hdr[1] = send_msg_size; /* original size signals "compressed" */
		} else {
			send_hdr[0] = send_msg_size;
			send_hdr[1] = 0; /* 0 = uncompressed */
		}
		PROF_SET_SENDRECV_CONTEXT(CAPE_SR_PHASE_SIZE, i, (int)partner, epoch);
		PROF_START(_t_size_xchg);
		cape_ucx_sendrecv(send_hdr, sizeof(send_hdr), partner,
		                  recv_hdr, sizeof(recv_hdr), partner,
		                  size_token);
		PROF_ACC(prof_allreduce_size_ns, _t_size_xchg);
		PROF_CLEAR_SENDRECV_CONTEXT();
		recv_msg_size = recv_hdr[0]; /* wire size */
		unsigned long recv_orig_size = recv_hdr[1];
#else /* !CAPE_COMPRESS */
		/* Exchange message sizes (step tag: i*2) */
		PROF_SET_SENDRECV_CONTEXT(CAPE_SR_PHASE_SIZE, i, (int)partner, epoch);
		PROF_START(_t_size_xchg);
		cape_ucx_sendrecv(&send_msg_size, sizeof(unsigned long), partner,
		                  &recv_msg_size, sizeof(unsigned long), partner,
		                  size_token);
		PROF_ACC(prof_allreduce_size_ns, _t_size_xchg);
		PROF_CLEAR_SENDRECV_CONTEXT();
#endif /* CAPE_COMPRESS */

		recv_msg = malloc(sizeof(char) * recv_msg_size);
		if (recv_msg == NULL) {
			fprintf(stderr, "CAPE hypercube_allreduce: malloc failed (recv_msg_size=%lu)\n",
			        recv_msg_size);
#ifdef CAPE_COMPRESS
			free(comp_buf);
#endif
			return -1;
		}

		/* Exchange checkpoint data (step tag: i*2+1) */
		PROF_SET_SENDRECV_CONTEXT(CAPE_SR_PHASE_DATA, i, (int)partner, epoch);
		PROF_START(_t_data_xchg);
#ifdef CAPE_COMPRESS
		cape_ucx_sendrecv(comp_buf ? comp_buf : send_msg,
		                  comp_buf ? comp_len : send_msg_size, partner,
		                  recv_msg, recv_msg_size, partner,
		                  data_token);
#else
		cape_ucx_sendrecv(send_msg, send_msg_size, partner,
		                  recv_msg, recv_msg_size, partner,
		                  data_token);
#endif
		PROF_ACC(prof_allreduce_data_ns, _t_data_xchg);
		PROF_CLEAR_SENDRECV_CONTEXT();

#ifdef CAPE_COMPRESS
		free(comp_buf);
		comp_buf = NULL;

		/* Decompress if the sender indicated compression */
		char *merge_buf = recv_msg;
		size_t merge_len = recv_msg_size;
		if (recv_orig_size > 0) {
			merge_buf = cape_decompress(recv_msg, recv_msg_size, &merge_len);
			free(recv_msg);
			if (!merge_buf) {
				fprintf(stderr, "CAPE hypercube_allreduce: decompression failed\n");
				return -1;
			}
		}
		PROF_START(_t_merge);
		rc = merge_checkpoint(merge_buf, merge_len, ckpt_flag);
		PROF_ACC(prof_merge_ckpt_ns, _t_merge);
		free(merge_buf);
#else
		PROF_START(_t_merge);
		rc = merge_checkpoint(recv_msg, recv_msg_size, ckpt_flag);
		PROF_ACC(prof_merge_ckpt_ns, _t_merge);
		free(recv_msg);
#endif
		recv_msg_size = 0;
		if (rc < 0)
			return -1;

		send_msg = after_ckpt;
		send_msg_size = after_ckpt_size;
	}

	return 1;
}

/*
 *---------------------------------------------------------------------
 * Send and Receive checkpoint based on Ring algorithm  
 *----------------------------------------------------------------------
 */
int ring_allreduce(unsigned int node, unsigned int nnodes, char ckpt_flag, uint32_t epoch){

	char *send_msg;
	char *recv_msg;
	char *owned_send_msg;
	unsigned int send_msg_size, recv_msg_size;
	int rc;
	uint32_t size_token, data_token;

	unsigned int i, left, right;
	left  = (node - 1 + nnodes) % nnodes;
	right = (node + 1) % nnodes;

	send_msg = after_ckpt;
	owned_send_msg = NULL;
	send_msg_size = after_ckpt_size;

	for(i = 1; i < nnodes; i++){
		int step = (int)(i - 1);
		size_token = ((epoch & 0x0fffU) << 8) | (uint32_t)((i * 2) & 0xff);
		data_token = ((epoch & 0x0fffU) << 8) | (uint32_t)(((i * 2) + 1) & 0xff);

#ifdef CAPE_COMPRESS
		char *comp_buf = NULL;
		size_t comp_len = 0;
		unsigned int send_hdr[3]; /* [0]=wire_size, [1]=orig_size_lo, [2]=orig_size_hi (0=uncompressed) */
		unsigned int recv_hdr[3];

		comp_buf = cape_compress(send_msg, send_msg_size, &comp_len);
		if (comp_buf) {
			send_hdr[0] = (unsigned int)comp_len;
			send_hdr[1] = send_msg_size;
			send_hdr[2] = 1; /* compressed flag */
		} else {
			send_hdr[0] = send_msg_size;
			send_hdr[1] = 0;
			send_hdr[2] = 0;
		}
		PROF_SET_SENDRECV_CONTEXT(CAPE_SR_PHASE_SIZE, step, (int)left, epoch);
		PROF_START(_t_size_xchg);
		cape_ucx_sendrecv(send_hdr, sizeof(send_hdr), right,
		                  recv_hdr, sizeof(recv_hdr), left,
		                  size_token);
		PROF_ACC(prof_allreduce_size_ns, _t_size_xchg);
		PROF_CLEAR_SENDRECV_CONTEXT();
		recv_msg_size = recv_hdr[0];
		unsigned int recv_orig_size = recv_hdr[1];
		unsigned int recv_compressed = recv_hdr[2];
#else /* !CAPE_COMPRESS */
		/* Exchange sizes: send to right, receive from left (step tag: i*2) */
		PROF_SET_SENDRECV_CONTEXT(CAPE_SR_PHASE_SIZE, step, (int)left, epoch);
		PROF_START(_t_size_xchg);
		cape_ucx_sendrecv(&send_msg_size, sizeof(unsigned int), right,
		                  &recv_msg_size, sizeof(unsigned int), left,
		                  size_token);
		PROF_ACC(prof_allreduce_size_ns, _t_size_xchg);
		PROF_CLEAR_SENDRECV_CONTEXT();
#endif /* CAPE_COMPRESS */

		recv_msg = malloc(sizeof(char) * recv_msg_size);
		if (recv_msg == NULL) {
			fprintf(stderr, "CAPE ring_allreduce: malloc failed (recv_msg_size=%u)\n",
			        recv_msg_size);
			if (owned_send_msg != NULL)
				free(owned_send_msg);
#ifdef CAPE_COMPRESS
			free(comp_buf);
#endif
			return -1;
		}

		/* Exchange data (step tag: i*2+1) */
		PROF_SET_SENDRECV_CONTEXT(CAPE_SR_PHASE_DATA, step, (int)left, epoch);
		PROF_START(_t_data_xchg);
#ifdef CAPE_COMPRESS
		cape_ucx_sendrecv(comp_buf ? comp_buf : send_msg,
		                  comp_buf ? (unsigned int)comp_len : send_msg_size, right,
		                  recv_msg, recv_msg_size, left,
		                  data_token);
#else
		cape_ucx_sendrecv(send_msg, send_msg_size, right,
		                  recv_msg, recv_msg_size, left,
		                  data_token);
#endif
		PROF_ACC(prof_allreduce_data_ns, _t_data_xchg);
		PROF_CLEAR_SENDRECV_CONTEXT();

#ifdef CAPE_COMPRESS
		free(comp_buf);
		comp_buf = NULL;

		char *merge_buf = recv_msg;
		size_t merge_len = recv_msg_size;
		if (recv_compressed && recv_orig_size > 0) {
			merge_buf = cape_decompress(recv_msg, recv_msg_size, &merge_len);
			free(recv_msg);
			recv_msg = NULL;
			if (!merge_buf) {
				fprintf(stderr, "CAPE ring_allreduce: decompression failed\n");
				if (owned_send_msg != NULL)
					free(owned_send_msg);
				return -1;
			}
		}

		PROF_START(_t_merge);
		rc = merge_checkpoint(merge_buf, merge_len, ckpt_flag);
		PROF_ACC(prof_merge_ckpt_ns, _t_merge);
		if (owned_send_msg != NULL) {
			free(owned_send_msg);
			owned_send_msg = NULL;
		}
		if (rc < 0) {
			free(merge_buf);
			return -1;
		}

		owned_send_msg = merge_buf;
		send_msg      = owned_send_msg;
		send_msg_size = (unsigned int)merge_len;
#else
		PROF_START(_t_merge);
		rc = merge_checkpoint(recv_msg, recv_msg_size, ckpt_flag);
		PROF_ACC(prof_merge_ckpt_ns, _t_merge);
		if (owned_send_msg != NULL) {
			free(owned_send_msg);
			owned_send_msg = NULL;
		}
		if (rc < 0) {
			free(recv_msg);
			return -1;
		}

		owned_send_msg = recv_msg;
		send_msg      = owned_send_msg;
		send_msg_size = recv_msg_size;
#endif
		recv_msg_size = 0;
	}
	if (owned_send_msg != NULL)
		free(owned_send_msg);
	return 1;
}

/*
 * ---------------------------------------------------------------------
 * Require synchronize checkpoints
 * ---------------------------------------------------------------------
 */

int require_allreduce(char ckpt_flag){
	uint32_t epoch;

	//printf("Node %d:  nnodes = %d - is_power_of2: %d \n", __node__, __nnodes__, is_power_of_two(__nnodes__));

	if (after_ckpt_size == 0) return 0 ;
	__allreduce_epoch__++;
	epoch = __allreduce_epoch__ & 0x0fffU;
	if (epoch == 0)
		epoch = 1;
	PROF_RECORD_ALLREDUCE_ARRIVAL(epoch);
	PROF_START(_t_allreduce);
	int _rc;
	if (is_power_of_two(__nnodes__))
		_rc = hypercube_allreduce(__node__, __nnodes__, ckpt_flag, epoch);
	else
		_rc = ring_allreduce(__node__, __nnodes__, ckpt_flag, epoch);
	PROF_ACC(prof_allreduce_total_ns, _t_allreduce);
	PROF_INC(prof_allreduce_count);
	return _rc;
}

/*
 * ---------------------------------------------------------------------
 * Inject checkpoint into application's memory
 * Parameters: checkpoint file
 * ---------------------------------------------------------------------
 */
int inject_checkpoint(char *data_ckpt, size_t file_size){
	PROF_START(_t_inject);
	const unsigned long ckpt_header_size = sizeof(unsigned long) * 3;

	if (file_size <= ckpt_header_size) {
		PROF_ACC(prof_inject_ckpt_ns, _t_inject);
		PROF_INC(prof_inject_ckpt_count);
		return 0;
	}

	__pc__ = *(unsigned long *)(data_ckpt + sizeof(unsigned long));
	unsigned long size_s = *(unsigned long *)(data_ckpt + 2 * sizeof(unsigned long));
	if (size_s > file_size) {
		fprintf(stderr, "CAPE inject_checkpoint: bad size_s=%lu file=%zu\n",
		        size_s, file_size);
		return -1;
	}

	size_t fp = ckpt_header_size;
	/* Bitmap format: walk var entries, then for each set bit in
	 * page_bmp, apply the corresponding [word_bmp][page_data] record
	 * by writing only the words whose word_bmp bit is set.
	 * Page-aligned writes — we touch each page exactly once per inject. */
	while (fp < size_s) {
		if ((size_s - fp) < 24) {
			fprintf(stderr, "CAPE inject_checkpoint: truncated bitmap header at %zu/%lu\n",
			        fp, size_s);
			return -1;
		}
		unsigned long var_addr = *(unsigned long *)(data_ckpt + fp); fp += 8;
		unsigned long var_size = *(unsigned long *)(data_ckpt + fp); fp += 8;
		uint32_t n_pages = *(uint32_t *)(data_ckpt + fp);            fp += 4;
		uint32_t n_dirty = *(uint32_t *)(data_ckpt + fp);            fp += 4;
		size_t page_bmp_bytes = ((size_t)n_pages + 7u) / 8u;
		size_t entries_bytes = (size_t)n_dirty * BMP_PAGE_ENTRY_BYTES;
		if ((size_s - fp) < page_bmp_bytes + entries_bytes) {
			fprintf(stderr, "CAPE inject_checkpoint: truncated bitmap payload\n");
			return -1;
		}
		const unsigned char *pbmp    = (const unsigned char *)data_ckpt + fp;
		fp += page_bmp_bytes;
		const unsigned char *entries = (const unsigned char *)data_ckpt + fp;
		fp += entries_bytes;
		(void)var_size;

		size_t ei = 0;
		for (uint32_t p = 0; p < n_pages; p++) {
			if (!bmp_get(pbmp, p))
				continue;
			const unsigned char *e = entries + ei * BMP_PAGE_ENTRY_BYTES;
			ei++;
			const unsigned char *wbmp = e;
			const unsigned char *pdata = e + BMP_WORD_BMP_BYTES;
			unsigned char *dst = (unsigned char *)(uintptr_t)
				(var_addr + ((size_t)p << BMP_PAGE_SHIFT));

			/* Fast path: if every word in this page is dirty,
			 * collapse to a single page-aligned 4 KB memcpy. The
			 * dense workload (write_stress) hits this >99% of
			 * the time. */
			int all_set = 1;
			for (size_t bi = 0; bi < BMP_WORD_BMP_BYTES; bi++) {
				if (wbmp[bi] != 0xFFu) { all_set = 0; break; }
			}
			if (all_set) {
				memcpy(dst, pdata, BMP_PAGE_SIZE);
				continue;
			}

			/* Sparse path: for each bitmap byte, fast-skip 0x00,
			 * memcpy 32 bytes for 0xFF, otherwise pick out the
			 * set words via __builtin_ctz. */
			int *dst32 = (int *)dst;
			const int *src32 = (const int *)pdata;
			for (size_t bi = 0; bi < BMP_WORD_BMP_BYTES; bi++) {
				unsigned char b = wbmp[bi];
				if (b == 0)
					continue;
				int *dst8 = dst32 + (bi << 3);
				const int *src8 = src32 + (bi << 3);
				if (b == 0xFFu) {
					memcpy(dst8, src8, 8 * sizeof(int));
					continue;
				}
				while (b) {
					int w = __builtin_ctz(b);
					dst8[w] = src8[w];
					b &= (unsigned char)(b - 1);
				}
			}
		}
	}
	PROF_ACC(prof_inject_ckpt_ns, _t_inject);
	PROF_INC(prof_inject_ckpt_count);
	return 1;
}

void release_checkpoint(){
	PROF_START(_t_rel);
	if (after_ckpt_stream != NULL){
		if (after_ckpt_stream) { fclose(after_ckpt_stream); after_ckpt_stream = NULL; }
		after_ckpt_stream = NULL;
	}
	after_ckpt_release();
	PROF_ACC(prof_release_ckpt_ns, _t_rel);
	PROF_INC(prof_release_count);
}


/***********************************************************************/
/* Runtime functions												   */
/***********************************************************************/
void cape_set_num_nodes(int nnodes){
	__nnodes__ = nnodes ;
}

void cape_set_time_stamp(int time_stamp){
	__time_stamp__ = time_stamp;
}

int cape_get_node_num(){
	return __node__;
}

int cape_get_num_nodes(){
	return __nnodes__;
}

int cape_get_left(int n, int start){
	int left = __node__ * (n - start) /__nnodes__ + start;
	return left ;
}

int cape_get_right(int n, int start){
	int right = (__node__ + 1) * (n - start) / __nnodes__ + start;	
	return right;
}

int cape_get_token(){
	return __cape_token__;
}

int cape_section(){
	__current_session__ ++ ; //declere section
	if (__current_session__ % __nnodes__ == __node__)
		return 1;
	else
		return 0;	
}


/*******************************************/
/* Data-sharing attribute */
/*******************************************/
/**
 * ---------------------------------------------------------------------
 * Set data attribute: default(none)
 * ---------------------------------------------------------------------
 */
void set_default_none(VarList *vlist_tail, char level){
	VarList *vtail;
	vtail = vlist_tail;
	while ((vtail != NULL ) && (vtail->var.level == level) ){
		vtail->var.pro = CAPE_PRIVATE;		
		vtail = vtail->prev;
	}
}

/**
 * ---------------------------------------------------------------------
 * Set data attribute
 * ---------------------------------------------------------------------
 */
void set_data_attribute(VarList *vlist_tail, long addr, char pro, char level){
	/* O(1) lookup via hash table */
	VarHashKey key;
	memset(&key, 0, sizeof(key));
	key.addr = (unsigned long)addr;
	key.level = (unsigned char)level;
	VarList *found = NULL;
	HASH_FIND(hh, __var_list_hash, &key, sizeof(VarHashKey), found);
	if (found != NULL) {
		found->var.pro = pro;
	}
}

/**
 * ---------------------------------------------------------------------
 * Set thread private
 * ---------------------------------------------------------------------
 */
void set_threadprivate(VarList *vlist_head, long addr){
	/* Search across all levels in the hash — threadprivate applies globally */
	VarList *vhead;
	vhead = vlist_head;
	while (vhead != NULL ){
		if (vhead->var.addr == addr) {
			vhead->var.pro = CAPE_PRIVATE;
			break;
		}
		vhead = vhead->next;
	}
}


/***********************************************************************/
/*		Clauses and data sharing directives							   */
/***********************************************************************/
void cape_set_default_none(){
	set_default_none(__var_list_tail, __parallel_level__);
}
void cape_set_threadprivate(void *addr){
	set_threadprivate(__var_list_head, (long)(uintptr_t)addr);
}
void cape_set_shared(void *addr){
	set_data_attribute(__var_list_tail, (long)(uintptr_t)addr, CAPE_SHARED, __parallel_level__);
}
void cape_set_private(void *addr){
	set_data_attribute(__var_list_tail, (long)(uintptr_t)addr, CAPE_PRIVATE, __parallel_level__);
}
void cape_set_firstprivate(void *addr){
	set_data_attribute(__var_list_tail, (long)(uintptr_t)addr, CAPE_FIRST_PRIVATE, __parallel_level__);
}
void cape_set_lastprivate(void *addr){
	set_data_attribute(__var_list_tail, (long)(uintptr_t)addr, CAPE_LAST_PRIVATE, __parallel_level__);
}
void cape_set_reduction(void *addr, char op){
	set_data_attribute(__var_list_tail, (long)(uintptr_t)addr, op , __parallel_level__);
}
void cape_set_copyin(void *addr){
	set_data_attribute(__var_list_tail, (long)(uintptr_t)addr, CAPE_COPY_IN, __parallel_level__);
}
void cape_set_copythread(void *addr){
	set_data_attribute(__var_list_tail, (long)(uintptr_t)addr, CAPE_COPY_PRIVATE, __parallel_level__);
}





/*****************************************/
void print_var_list(VarList *vlist)
{
	printf("PRINT ACTIVE VARIABLE LIST: \n");
	if(vlist != NULL){
		while(vlist!=NULL){
			printf("Node %ld: Variables: Ox%lx - size: %u - n: %u- pro: %d - dtype: %d - level: %d - is pointer: %d \n ", __node__, 	 \
					vlist->var.addr, 
					vlist->var.size, 
					vlist->var.n, 
					vlist->var.pro, 
					vlist->var.dtype,
					vlist->var.level,
					vlist->var.ispointer );
			vlist = vlist->next;
		}
	}
	else
		printf("VarList is NULL \n");
}

void print_var_head_list(PointerList *vlist){
	if(vlist != NULL){
		while(vlist!=NULL){
			printf("\n Node %d: In Heap: Manager addr: Ox%lx - Addr: 0x%lx - %ld bytes \n ", __node__, 	 \
					vlist->pointer.manager_addr, vlist->pointer.addr, vlist->pointer.len);
			vlist = vlist->next;
		}
	}
	else
		printf("VarList is NULL \n");
}



int print_data_in_checkpoint(char *data_ckpt, size_t file_size){
	unsigned long len=0, file_pointer = 0;
	unsigned long addr, pc, size_s, timestamp, i;
	
	timestamp = *(unsigned long *) data_ckpt ;
	pc = *(unsigned long *)(data_ckpt+ 4); //get program counter
	size_s =  *(unsigned long *)(data_ckpt+ 8);
	
	file_pointer = 12 ;
	
	printf("\nNode: %ld - timestamp: %ld - PC: Ox%lx - Size_S: %ld \n", __node__, timestamp, pc, size_s );
	
	while(file_pointer < file_size){
		
		addr = *(long *) (data_ckpt + file_pointer);		
		file_pointer += sizeof(long);		
		
		len = *(unsigned int*) (data_ckpt + file_pointer );
		file_pointer += sizeof(long);			
		
		printf(" ------ Node: %ld - Addr: 0x%lx - Len: %ld - Data: ", __node__, addr, len );
		for(i = 0; i < len/4 ; i++ ){
			printf("\t%d", *(int *) (data_ckpt + file_pointer));
			file_pointer += sizeof(int);
		}
		
		
		//if (__node__ == 0)
		//	printf("DATA: Node %d: addr = 0x%lx - len = %d bytes - data %d \n",__node__, addr, len, *(int *) (data_ckpt+ file_pointer) );	
				
		//file_pointer += len ;			
	}
	
	return 1;
}


/****************************************************************************************
 * public functions
 * **************************************************************************************
 */

/*
 * cape_declare_variable: version 7.0
 * 	Save all active and shared variables in __active_variable list
 *  (global and local variable is declared outside #pragma omp parallel)
 */
int cape_declare_variable(void *addr,
						  unsigned char dtype,
						  unsigned int n_elements,
						  unsigned char ispointer){
	PROF_START(_t_decl);
	if (__is_inside_parallel_region__) {
		PROF_ACC(prof_declare_var_ns, _t_decl);
		PROF_INC(prof_declare_var_count);
		return 0;
	}

	Var v;
	v.addr = (unsigned long)(uintptr_t)addr;
	v.dtype = dtype;
	v.n = n_elements;
	v.pro = CAPE_SHARED;
	v.level = __activate_func_level__ ;
	v.ispointer = ispointer;
	unsigned int size;
	if (ispointer){
		size = 4 ;
	}
	else
	{
		switch (dtype){
			case CAPE_CHAR:
			case CAPE_UNSIGNED_CHAR:
				size = 1;
				break;
			case CAPE_INT:
			case CAPE_UNSIGNED_INT:
			case CAPE_LONG:
			case CAPE_UNSIGNED_LONG:
			case CAPE_FLOAT:
				size = 4 ;
				break;
			case CAPE_DOUBLE:
				size = 8 ;
				break;
			default:
				size = 4;
		}
	}
	v.size = size;
	int val;
	val = add_active_variable(&__active_variable_head, &__active_variable_tail, v);
	PROF_ACC(prof_declare_var_ns, _t_decl);
	PROF_INC(prof_declare_var_count);
	return val;
}

/*
 * ---------------------------------------------------------------------
 * Declare a initialization of heap memory
 * v = cape_malloc(size)
 * v = cape_calloc(v, p, size);
 * ---------------------------------------------------------------------
 */
void cape_allocate_memory(unsigned long manager_addr, 
						unsigned long addr, unsigned long nbytes){
	Pointer pt ;	
	pt.manager_addr = manager_addr ;
	pt.addr = addr ;
	pt.len = nbytes;	
	remove_exists_item(&__var_heap_list_head, &__var_heap_list_tail, pt);
	addnew_pointer(&__var_heap_list_head, &__var_heap_list_tail, pt) ;
		
} 
 /*
  *  ---------------------------------------------------------------------
  * Re-allocate heap memory
  * v = cape_reaalloc(p, size)
  * ---------------------------------------------------------------------
  */
void cape_reallocate_memory(unsigned long manager_addr, unsigned long old_addr,
						unsigned long addr, unsigned long nbytes){
	Pointer pt, pt_new ;	
	pt.manager_addr = manager_addr ;
	pt.addr = old_addr ;
	pt.len = 4;	
	remove_exists_item(&__var_heap_list_head, &__var_heap_list_tail, pt);
	
	pt_new.manager_addr = manager_addr;
	pt_new.addr = addr ;
	pt_new.len = nbytes ;
	addnew_pointer(&__var_heap_list_head, &__var_heap_list_tail, pt) ;
		
} 

/*
 * ---------------------------------------------------------------------
 * free(v);
 * ---------------------------------------------------------------------
 */
void cape_free_memory(unsigned long manager_addr){
	remove_heap_variables(&__var_heap_list_head,
						&__var_heap_list_tail,
						manager_addr);	
} 

/*
 * ---------------------------------------------------------------------
 * TODO: Initialize 
 * Version 7.0
 * ---------------------------------------------------------------------
 */
/*
 * Bootstrap helper: exchange UCX worker addresses via files on a shared
 * filesystem.  Each rank writes its address to a file, signals readiness,
 * then waits until every other rank has done the same before reading peers.
 *
 * Files are named  <dir>/cape_ucx_<jobid>_addr_<rank>
 *                  <dir>/cape_ucx_<jobid>_rdy_<rank>
 * so multiple concurrent jobs never collide.
 */
static void ucx_exchange_addresses_via_fs(const char *dir, const char *jobid,
                                          ucp_address_t *local_addr,
                                          size_t local_addr_len)
{
	char path[512];
	FILE *f;
#ifdef CAPE_PROFILE
	double _fs_total_start = cape_get_ns();
#endif

	const size_t expected_size = sizeof(size_t) + local_addr_len;

	/* Write own address (fsync before signaling ready, otherwise peers on a
	 * network FS may see the rdy flag before our addr bytes are visible and
	 * read garbage as the peer length/contents). */
#ifdef CAPE_PROFILE
	double _addr_write_start = cape_get_ns();
#endif
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%d", dir, jobid, __node__);
	f = fopen(path, "wb");
	if (!f) { perror("CAPE UCX: write addr file"); exit(1); }
	if (fwrite(&local_addr_len, sizeof(size_t), 1, f) != 1 ||
	    fwrite(local_addr, local_addr_len, 1, f) != 1) {
		perror("CAPE UCX: short write on addr file"); exit(1);
	}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
#ifdef CAPE_PROFILE
	double _addr_write_ns = cape_get_ns() - _addr_write_start;
	prof_ucx_bootstrap_fs_addr_ns += _addr_write_ns;
	if (cape_ucx_diag_is_slow(_addr_write_ns)) {
		cape_ucx_diag_log("fs_write_addr_ms=%.3f dir=%s job=%s bytes=%zu\n",
		                  _addr_write_ns / 1e6, dir, jobid, expected_size);
	}
#endif

	/* Signal ready (only after the addr bytes are durable). */
#ifdef CAPE_PROFILE
	double _rdy_write_start = cape_get_ns();
#endif
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%d", dir, jobid, __node__);
	f = fopen(path, "w");
	if (!f) { perror("CAPE UCX: write rdy file"); exit(1); }
	fflush(f);
	fsync(fileno(f));
	fclose(f);
#ifdef CAPE_PROFILE
	double _rdy_write_ns = cape_get_ns() - _rdy_write_start;
	prof_ucx_bootstrap_fs_rdy_ns += _rdy_write_ns;
	if (cape_ucx_diag_is_slow(_rdy_write_ns)) {
		cape_ucx_diag_log("fs_write_rdy_ms=%.3f dir=%s job=%s\n",
		                  _rdy_write_ns / 1e6, dir, jobid);
	}
#endif

	/* Wait for all peers' rdy flags */
#ifdef CAPE_PROFILE
	double _wait_all_start = cape_get_ns();
#endif
	for (int i = 0; i < __nnodes__; i++) {
#ifdef CAPE_PROFILE
		double _wait_peer_start = cape_get_ns();
		unsigned long _wait_peer_iters = 0;
#endif
		snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%d", dir, jobid, i);
		while (access(path, F_OK) != 0) {
#ifdef CAPE_PROFILE
			_wait_peer_iters++;
			prof_ucx_bootstrap_fs_wait_iters++;
#endif
			usleep(10000); /* 10 ms */
		}
#ifdef CAPE_PROFILE
		double _wait_peer_ns = cape_get_ns() - _wait_peer_start;
		if (cape_ucx_diag_is_slow(_wait_peer_ns)) {
			cape_ucx_diag_log("fs_wait_rdy_ms=%.3f peer=%d iters=%lu path=%s\n",
			                  _wait_peer_ns / 1e6, i, _wait_peer_iters, path);
		}
#endif
	}
#ifdef CAPE_PROFILE
	prof_ucx_bootstrap_fs_wait_ns += cape_get_ns() - _wait_all_start;
#endif

	/* Create endpoints */
	ucs_status_t st;
	ucp_endpoints = malloc(__nnodes__ * sizeof(ucp_ep_h));
	for (int i = 0; i < __nnodes__; i++) {
#ifdef CAPE_PROFILE
		double _read_start = cape_get_ns();
#endif
		snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%d", dir, jobid, i);

		/* On a network FS the addr file may exist but not yet contain all
		 * bytes from the peer when we observe the rdy flag. Wait until at
		 * least sizeof(size_t) is visible, then trust the embedded length
		 * and wait for the rest. */
		struct stat stbuf;
		while (stat(path, &stbuf) != 0 || (size_t)stbuf.st_size < sizeof(size_t))
			usleep(10000);

		f = fopen(path, "rb");
		if (!f) { perror("CAPE UCX: read peer addr file"); exit(1); }
		size_t peer_len = 0;
		if (fread(&peer_len, sizeof(size_t), 1, f) != 1) {
			fprintf(stderr, "CAPE UCX: short read of peer %d length\n", i);
			exit(1);
		}
		if (peer_len == 0 || peer_len > (1u << 20)) {
			fprintf(stderr, "CAPE UCX: implausible peer %d addr length %zu\n",
			        i, peer_len);
			exit(1);
		}
		fclose(f);

		const size_t want = sizeof(size_t) + peer_len;
		while (stat(path, &stbuf) != 0 || (size_t)stbuf.st_size < want)
			usleep(10000);

		f = fopen(path, "rb");
		if (!f) { perror("CAPE UCX: reopen peer addr file"); exit(1); }
		size_t verify_len = 0;
		if (fread(&verify_len, sizeof(size_t), 1, f) != 1 || verify_len != peer_len) {
			fprintf(stderr, "CAPE UCX: peer %d length changed (%zu -> %zu)\n",
			        i, peer_len, verify_len);
			exit(1);
		}
		ucp_address_t *peer_addr = malloc(peer_len);
		size_t got = 0;
		while (got < peer_len) {
			size_t n = fread((char *)peer_addr + got, 1, peer_len - got, f);
			if (n == 0) {
				if (feof(f)) usleep(10000);
				else { perror("CAPE UCX: read peer addr"); exit(1); }
				clearerr(f);
			}
			got += n;
		}
		fclose(f);
#ifdef CAPE_PROFILE
		double _read_ns = cape_get_ns() - _read_start;
		prof_ucx_bootstrap_fs_read_ns += _read_ns;
		if (cape_ucx_diag_is_slow(_read_ns)) {
			cape_ucx_diag_log("fs_read_addr_ms=%.3f peer=%d peer_len=%zu path=%s\n",
			                  _read_ns / 1e6, i, peer_len, path);
		}
#endif

		ucp_ep_params_t ep_params;
		memset(&ep_params, 0, sizeof(ep_params));
		ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
		ep_params.address    = peer_addr;
#ifdef CAPE_PROFILE
		double _ep_start = cape_get_ns();
#endif
		st = ucp_ep_create(ucp_worker, &ep_params, &ucp_endpoints[i]);
#ifdef CAPE_PROFILE
		double _ep_ns = cape_get_ns() - _ep_start;
		prof_ucx_bootstrap_ep_create_ns += _ep_ns;
		prof_ucx_bootstrap_ep_create_count++;
		if (cape_ucx_diag_is_slow(_ep_ns)) {
			cape_ucx_diag_log("ucp_ep_create_ms=%.3f peer=%d peer_len=%zu local_len=%zu\n",
			                  _ep_ns / 1e6, i, peer_len, local_addr_len);
		}
#endif
		free(peer_addr);
		if (st != UCS_OK) {
			fprintf(stderr, "CAPE UCX: ucp_ep_create(%d) failed: %s peer_len=%zu local_len=%zu\n",
			        i, ucs_status_string(st), peer_len, local_addr_len);
			exit(1);
		}
	}
#ifdef CAPE_PROFILE
	double _fs_total_ns = cape_get_ns() - _fs_total_start;
	prof_ucx_bootstrap_fs_ns += _fs_total_ns;
	cape_ucx_diag_log(
	    "fs_bootstrap_total_ms=%.3f write_addr_ms=%.3f write_rdy_ms=%.3f "
	    "wait_rdy_ms=%.3f read_addr_ms=%.3f ep_create_ms=%.3f wait_iters=%lu dir=%s job=%s\n",
	    _fs_total_ns / 1e6,
	    prof_ucx_bootstrap_fs_addr_ns / 1e6,
	    prof_ucx_bootstrap_fs_rdy_ns / 1e6,
	    prof_ucx_bootstrap_fs_wait_ns / 1e6,
	    prof_ucx_bootstrap_fs_read_ns / 1e6,
	    prof_ucx_bootstrap_ep_create_ns / 1e6,
	    prof_ucx_bootstrap_fs_wait_iters,
	    dir, jobid);
#endif
}

void cape_init(){
#ifdef CAPE_PROFILE
	double _cape_init_start = cape_get_ns();
	cape_ucx_diag_configure();
#endif
	/* Keep default CAPE messages on eager path unless user explicitly overrides.
	 * This avoids rendezvous metadata appearing in the receive buffer on some
	 * UCX/IB setups when checkpoint payload is around 10KB. */
	setenv("UCX_RNDV_THRESH", "inf", 0);

	/* ------------------------------------------------------------------
	 * 1. Get rank and size.
	 *    With USE_PMIX: use the PMIx API (preferred, works with OpenMPI).
	 *    Without      : read SLURM_PROCID / SLURM_NTASKS set by srun.
	 * ------------------------------------------------------------------ */
#ifdef USE_PMIX
	PMIX_PROC_CONSTRUCT(&pmix_myproc);
	pmix_status_t pst = PMIx_Init(&pmix_myproc, NULL, 0);
	if (pst != PMIX_SUCCESS) {
		fprintf(stderr, "CAPE UCX: PMIx_Init failed: %s\n",
		        PMIx_Error_string(pst));
		exit(1);
	}
	__node__ = (int)pmix_myproc.rank;

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
	__nnodes__ = (int)val->data.uint32;
	PMIX_VALUE_RELEASE(val);
#else
	/* srun sets SLURM_PROCID and SLURM_NTASKS for every task */
	const char *rank_str = getenv("SLURM_PROCID");
	const char *size_str = getenv("SLURM_NTASKS");
	/* Also accept OpenMPI env vars when running under mpirun locally */
	if (!rank_str) rank_str = getenv("OMPI_COMM_WORLD_RANK");
	if (!size_str) size_str = getenv("OMPI_COMM_WORLD_SIZE");
	if (!rank_str || !size_str) {
		fprintf(stderr, "CAPE UCX: cannot determine rank/size. "
		        "Run via srun (sets SLURM_PROCID/SLURM_NTASKS) "
		        "or compile with -DUSE_PMIX.\n");
		exit(1);
	}
	__node__   = atoi(rank_str);
	__nnodes__ = atoi(size_str);
#endif
#ifdef CAPE_PROFILE
	cape_ucx_diag_log(
	    "init_rank rank=%d size=%d UCX_RNDV_THRESH=%s UCX_TLS=%s "
	    "UCX_NET_DEVICES=%s UCX_IB_REG_METHODS=%s UCX_REG_NONBLOCK_MEM_TYPES=%s "
	    "UCX_PROTO_ENABLE=%s\n",
	    __node__, __nnodes__,
	    cape_env_or_dash("UCX_RNDV_THRESH"),
	    cape_env_or_dash("UCX_TLS"),
	    cape_env_or_dash("UCX_NET_DEVICES"),
	    cape_env_or_dash("UCX_IB_REG_METHODS"),
	    cape_env_or_dash("UCX_REG_NONBLOCK_MEM_TYPES"),
	    cape_env_or_dash("UCX_PROTO_ENABLE"));
#endif

	/* ------------------------------------------------------------------
	 * 2. Initialise UCX context.
	 * ------------------------------------------------------------------ */
	ucp_params_t ucp_params;
	memset(&ucp_params, 0, sizeof(ucp_params));
	ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES
	                        | UCP_PARAM_FIELD_REQUEST_SIZE
	                        | UCP_PARAM_FIELD_REQUEST_INIT;
	ucp_params.features     = UCP_FEATURE_TAG;
	ucp_params.request_size = sizeof(cape_ucx_req_t);
	ucp_params.request_init = cape_ucx_req_init;

	ucp_config_t *config;
#ifdef CAPE_PROFILE
	double _config_start = cape_get_ns();
#endif
	ucp_config_read(NULL, NULL, &config);
#ifdef CAPE_PROFILE
	double _config_ns = cape_get_ns() - _config_start;
	prof_ucx_config_ns += _config_ns;
#endif
#ifdef CAPE_PROFILE
	double _ucp_init_start = cape_get_ns();
#endif
	ucs_status_t st = ucp_init(&ucp_params, config, &ucp_context);
#ifdef CAPE_PROFILE
	double _ucp_init_ns = cape_get_ns() - _ucp_init_start;
	prof_ucp_init_ns += _ucp_init_ns;
	cape_ucx_diag_log("ucp_config_ms=%.3f ucp_init_ms=%.3f\n",
	                  _config_ns / 1e6, _ucp_init_ns / 1e6);
#endif
	ucp_config_release(config);
	if (st != UCS_OK) {
		fprintf(stderr, "CAPE UCX: ucp_init failed: %s\n",
		        ucs_status_string(st));
		exit(1);
	}

	/* ------------------------------------------------------------------
	 * 3. Create a single-threaded UCX worker.
	 * ------------------------------------------------------------------ */
	ucp_worker_params_t wp;
	memset(&wp, 0, sizeof(wp));
	wp.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
	wp.thread_mode = UCS_THREAD_MODE_SINGLE;
#ifdef CAPE_PROFILE
	double _worker_start = cape_get_ns();
#endif
	st = ucp_worker_create(ucp_context, &wp, &ucp_worker);
#ifdef CAPE_PROFILE
	double _worker_ns = cape_get_ns() - _worker_start;
	prof_ucp_worker_ns += _worker_ns;
	cape_ucx_diag_log("ucp_worker_create_ms=%.3f\n", _worker_ns / 1e6);
#endif
	if (st != UCS_OK) {
		fprintf(stderr, "CAPE UCX: ucp_worker_create failed: %s\n",
		        ucs_status_string(st));
		exit(1);
	}

	/* ------------------------------------------------------------------
	 * 4. Exchange worker addresses and build endpoints.
	 *    With USE_PMIX: use the PMIx KVS + Fence (in-memory, fast).
	 *    Without      : use files on the shared filesystem.
	 * ------------------------------------------------------------------ */
	ucp_address_t *local_addr;
	size_t         local_addr_len;
#ifdef CAPE_PROFILE
	double _worker_addr_start = cape_get_ns();
#endif
	ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_len);
#ifdef CAPE_PROFILE
	double _worker_addr_ns = cape_get_ns() - _worker_addr_start;
	prof_ucp_worker_addr_ns += _worker_addr_ns;
	cape_ucx_diag_log("ucp_worker_get_address_ms=%.3f local_addr_len=%zu\n",
	                  _worker_addr_ns / 1e6, local_addr_len);
#endif

#ifdef USE_PMIX
#ifdef CAPE_PROFILE
	double _pmix_bootstrap_start = cape_get_ns();
#endif
	char pmix_key[64];
	snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%d", __node__);

	pmix_value_t kval;
	PMIX_VALUE_CONSTRUCT(&kval);
	kval.type          = PMIX_BYTE_OBJECT;
	kval.data.bo.bytes = (char *)local_addr;
	kval.data.bo.size  = local_addr_len;

#ifdef CAPE_PROFILE
	double _pmix_put_start = cape_get_ns();
#endif
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
#ifdef CAPE_PROFILE
	prof_ucx_bootstrap_pmix_put_ns += cape_get_ns() - _pmix_put_start;
	double _pmix_fence_start = cape_get_ns();
#endif
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
#ifdef CAPE_PROFILE
	prof_ucx_bootstrap_pmix_fence_ns += cape_get_ns() - _pmix_fence_start;
#endif

	ucp_endpoints = malloc(__nnodes__ * sizeof(ucp_ep_h));
	for (int i = 0; i < __nnodes__; i++) {
		pmix_proc_t peer;
		PMIX_PROC_CONSTRUCT(&peer);
		PMIX_LOAD_NSPACE(peer.nspace, pmix_myproc.nspace);
		peer.rank = (pmix_rank_t)i;

		snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%d", i);
		pmix_value_t *peer_val;
#ifdef CAPE_PROFILE
		double _pmix_get_start = cape_get_ns();
#endif
		pst = PMIx_Get(&peer, pmix_key, NULL, 0, &peer_val);
#ifdef CAPE_PROFILE
		prof_ucx_bootstrap_pmix_get_ns += cape_get_ns() - _pmix_get_start;
#endif
		if (pst != PMIX_SUCCESS) {
			fprintf(stderr, "CAPE UCX: PMIx_Get(addr, rank=%d) failed: %s\n",
			        i, PMIx_Error_string(pst));
			exit(1);
		}
		ucp_ep_params_t ep_params;
		memset(&ep_params, 0, sizeof(ep_params));
		ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
		ep_params.address    = (ucp_address_t *)peer_val->data.bo.bytes;
#ifdef CAPE_PROFILE
		double _ep_start = cape_get_ns();
#endif
		st = ucp_ep_create(ucp_worker, &ep_params, &ucp_endpoints[i]);
#ifdef CAPE_PROFILE
		double _ep_ns = cape_get_ns() - _ep_start;
		prof_ucx_bootstrap_ep_create_ns += _ep_ns;
		prof_ucx_bootstrap_ep_create_count++;
		if (cape_ucx_diag_is_slow(_ep_ns)) {
			cape_ucx_diag_log("ucp_ep_create_ms=%.3f peer=%d peer_len=%zu local_len=%zu\n",
			                  _ep_ns / 1e6, i, peer_val->data.bo.size, local_addr_len);
		}
#endif
		PMIX_VALUE_RELEASE(peer_val);
		if (st != UCS_OK) {
			fprintf(stderr, "CAPE UCX: ucp_ep_create(%d) failed: %s\n",
			        i, ucs_status_string(st));
			exit(1);
		}
	}
#ifdef CAPE_PROFILE
	prof_ucx_bootstrap_pmix_ns += cape_get_ns() - _pmix_bootstrap_start;
	cape_ucx_diag_log(
	    "pmix_bootstrap_total_ms=%.3f put_commit_ms=%.3f fence_ms=%.3f get_ms=%.3f ep_create_ms=%.3f\n",
	    prof_ucx_bootstrap_pmix_ns / 1e6,
	    prof_ucx_bootstrap_pmix_put_ns / 1e6,
	    prof_ucx_bootstrap_pmix_fence_ns / 1e6,
	    prof_ucx_bootstrap_pmix_get_ns / 1e6,
	    prof_ucx_bootstrap_ep_create_ns / 1e6);
#endif
#else
	/* Use shared filesystem for rendezvous.
	 * Namespace files by SLURM_JOB_ID + SLURM_STEP_ID so concurrent srun
	 * steps inside the same allocation (e.g. parallel reps from the
	 * benchmark script) don't all write to the same _addr_<rank> file.
	 * SLURM_SUBMIT_DIR is the shared NFS directory sbatch was run from. */
	const char *job_str  = getenv("SLURM_JOB_ID");
	const char *step_str = getenv("SLURM_STEP_ID");
	const char *sharedir = getenv("SLURM_SUBMIT_DIR");
	char jobid_buf[64];
	if (!job_str)  job_str  = "local";
	if (!step_str) step_str = "0";
	if (!sharedir) sharedir = ".";
	snprintf(jobid_buf, sizeof(jobid_buf), "%s_%s", job_str, step_str);
	ucx_exchange_addresses_via_fs(sharedir, jobid_buf, local_addr, local_addr_len);
#endif

	ucp_worker_release_address(ucp_worker, local_addr);
#ifdef CAPE_PROFILE
	prof_cape_init_ns += cape_get_ns() - _cape_init_start;
	cape_ucx_diag_log("cape_init_total_ms=%.3f\n", prof_cape_init_ns / 1e6);
#endif
}

/*
 * ---------------------------------------------------------------------
 * Release the UCX environment (and PMIx if enabled).
 * ---------------------------------------------------------------------
 */
void cape_finalize(){
#ifdef CAPE_PROFILE
	/* ---- Performance profiling summary ---- */
	unsigned int s;
	double allreduce_comm_ns = prof_allreduce_size_ns + prof_allreduce_data_ns;
	double allreduce_other_ns = prof_allreduce_total_ns - allreduce_comm_ns - prof_merge_ckpt_ns;
	double arrival_span_ms = 0.0;
	unsigned long shown_arrivals = prof_allreduce_arrival_count;
	if (shown_arrivals > CAPE_PROFILE_MAX_ARRIVALS)
		shown_arrivals = CAPE_PROFILE_MAX_ARRIVALS;
	if (prof_allreduce_arrival_count > 1)
		arrival_span_ms = (prof_allreduce_arrival_last_rt_ns - prof_allreduce_arrival_first_rt_ns) / 1e6;

	fprintf(stderr,
	    "\n[CAPE PROFILE] Node %d  (all times in ms, counts in parentheses)\n"
	    "  ckpt_start   (snapshot vars)  : %10.3f ms  (x%lu)\n"
	    "  generate_ckpt (build delta)   : %10.3f ms  (x%lu)\n"
	    "  inject_ckpt  (write to mem)   : %10.3f ms  (x%lu)\n"
	    "  allreduce    (total)           : %10.3f ms  (x%lu)\n"
	    "    size exchange (sendrecv)     : %10.3f ms\n"
	    "    data exchange (sendrecv)     : %10.3f ms\n"
	    "    merge_checkpoint             : %10.3f ms\n"
	    "    other (malloc/free/overhead) : %10.3f ms\n"
	    "  UCX recv wait (network lat)   : %10.3f ms  (x%lu sendrecvs)\n"
	    "  UCX send wait (send compl)    : %10.3f ms\n"
	    "  -- UCX init/bootstrap --\n"
	    "  cape_init   (total)            : %10.3f ms\n"
	    "    ucp_config_read              : %10.3f ms\n"
	    "    ucp_init                     : %10.3f ms\n"
	    "    ucp_worker_create            : %10.3f ms\n"
	    "    ucp_worker_get_address       : %10.3f ms\n"
	    "    fs bootstrap total           : %10.3f ms\n"
	    "      write_addr=%10.3f  write_rdy=%10.3f  wait_rdy=%10.3f  read_addr=%10.3f  wait_iters=%lu\n"
	    "    pmix bootstrap total         : %10.3f ms\n"
	    "      put_commit=%10.3f  fence=%10.3f  get=%10.3f\n"
	    "    ucp_ep_create total          : %10.3f ms  (x%lu)\n"
	    "  allreduce arrival (realtime)  : first=%13.3f ms  last=%13.3f ms  span=%10.3f ms  (x%lu)\n"
	    "  Data transferred: sent=%lu B  recv=%lu B\n"
	    "  -- wider regions --\n"
	    "  cape_begin   (wall)            : %10.3f ms  (x%lu)\n"
	    "  cape_end     (wall, full)      : %10.3f ms  (x%lu)\n"
	    "  ckpt_stop    (wall)            : %10.3f ms  (x%lu)\n"
	    "  release_ckpt (wall)            : %10.3f ms  (x%lu)\n"
	    "  cape_sync_ckpt (allreduce+inj) : %10.3f ms  (x%lu)\n"
	    "  USER COMPUTE between ckpts     : %10.3f ms  (x%lu)\n"
	    "  declare_variable (total)        : %10.3f ms  (x%lu)\n"
	    "  __enter_func+__exit_func       : %10.3f ms  (x%lu)\n",
	    __node__,
	    prof_ckpt_start_ns    / 1e6, prof_ckpt_start_count,
	    prof_generate_ckpt_ns / 1e6, prof_generate_ckpt_count,
	    prof_inject_ckpt_ns   / 1e6, prof_inject_ckpt_count,
	    prof_allreduce_total_ns / 1e6, prof_allreduce_count,
	    prof_allreduce_size_ns  / 1e6,
	    prof_allreduce_data_ns  / 1e6,
	    prof_merge_ckpt_ns      / 1e6,
	    allreduce_other_ns      / 1e6,
	    prof_ucx_recv_wait_ns  / 1e6, prof_sendrecv_count,
	    prof_ucx_send_wait_ns  / 1e6,
	    prof_cape_init_ns / 1e6,
	    prof_ucx_config_ns / 1e6,
	    prof_ucp_init_ns / 1e6,
	    prof_ucp_worker_ns / 1e6,
	    prof_ucp_worker_addr_ns / 1e6,
	    prof_ucx_bootstrap_fs_ns / 1e6,
	    prof_ucx_bootstrap_fs_addr_ns / 1e6,
	    prof_ucx_bootstrap_fs_rdy_ns / 1e6,
	    prof_ucx_bootstrap_fs_wait_ns / 1e6,
	    prof_ucx_bootstrap_fs_read_ns / 1e6,
	    prof_ucx_bootstrap_fs_wait_iters,
	    prof_ucx_bootstrap_pmix_ns / 1e6,
	    prof_ucx_bootstrap_pmix_put_ns / 1e6,
	    prof_ucx_bootstrap_pmix_fence_ns / 1e6,
	    prof_ucx_bootstrap_pmix_get_ns / 1e6,
	    prof_ucx_bootstrap_ep_create_ns / 1e6,
	    prof_ucx_bootstrap_ep_create_count,
	    prof_allreduce_arrival_first_rt_ns / 1e6,
	    prof_allreduce_arrival_last_rt_ns / 1e6,
	    arrival_span_ms, prof_allreduce_arrival_count,
	    prof_bytes_sent, prof_bytes_received,
	    prof_cape_begin_ns    / 1e6, prof_cape_begin_count,
	    prof_cape_end_ns      / 1e6, prof_cape_end_count,
	    prof_ckpt_stop_ns     / 1e6, prof_ckpt_stop_count,
	    prof_release_ckpt_ns  / 1e6, prof_release_count,
	    prof_sync_ckpt_ns     / 1e6, prof_sync_count,
	    prof_compute_ns       / 1e6, prof_compute_count,
	    prof_declare_var_ns   / 1e6, prof_declare_var_count,
	    prof_enter_exit_func_ns / 1e6, prof_enter_exit_count);

	if (shown_arrivals > 0) {
		fprintf(stderr, "  allreduce arrivals by epoch (realtime ms):\n");
		for (s = 0; s < shown_arrivals; s++) {
			fprintf(stderr, "    #%u epoch=%u  t=%13.3f\n",
			        s + 1,
			        prof_allreduce_arrival_epoch[s],
			        prof_allreduce_arrival_rt_ns[s] / 1e6);
		}
		if (prof_allreduce_arrival_count > shown_arrivals) {
			fprintf(stderr, "    ... %lu more arrivals not shown\n",
			        prof_allreduce_arrival_count - shown_arrivals);
		}
	}

	if (prof_step_max_seen > 0) {
		fprintf(stderr, "  allreduce step recv-wait breakdown:\n");
		for (s = 0; s < prof_step_max_seen; s++) {
			int partner = prof_step_partner_set[s] ? prof_step_partner[s] : -1;
			fprintf(stderr,
			    "    step %u partner=%d  size_wait=%10.3f ms / size_total=%10.3f ms (x%lu, max=%10.3f ms @epoch %u)"
			    "  data_wait=%10.3f ms / data_total=%10.3f ms (x%lu, max=%10.3f ms @epoch %u)\n",
			    s, partner,
			    prof_step_size_recv_wait_ns[s] / 1e6,
			    prof_step_size_call_ns[s] / 1e6,
			    prof_step_size_count[s],
			    prof_step_size_max_recv_wait_ns[s] / 1e6, prof_step_size_max_epoch[s],
			    prof_step_data_recv_wait_ns[s] / 1e6,
			    prof_step_data_call_ns[s] / 1e6,
			    prof_step_data_count[s],
			    prof_step_data_max_recv_wait_ns[s] / 1e6, prof_step_data_max_epoch[s]);
		}
		if (prof_step_overflow > 0) {
			fprintf(stderr, "    ... %lu profiled step events exceeded CAPE_PROFILE_MAX_STEPS=%d\n",
			        prof_step_overflow, CAPE_PROFILE_MAX_STEPS);
		}
	}
#endif /* CAPE_PROFILE */

	/* Flush and close all endpoints */
	ucp_request_param_t close_param;
	memset(&close_param, 0, sizeof(close_param));
	for (int i = 0; i < __nnodes__; i++) {
		void *req = ucp_ep_close_nbx(ucp_endpoints[i], &close_param);
		if (UCS_PTR_IS_ERR(req)) {
			/* endpoint may already be in error state – skip */
			continue;
		}
		if (req == NULL)
			continue; /* completed immediately */
		/* Spin until close completes using ucp_request_check_status —
		 * ep close does not fire send/recv callbacks so r->completed
		 * is never set; poll the request status directly instead. */
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
#else
	/* Remove this rank's rendezvous files (must match the jobid scheme used
	 * in cape_init: SLURM_JOB_ID + "_" + SLURM_STEP_ID). */
	const char *job_str  = getenv("SLURM_JOB_ID");
	const char *step_str = getenv("SLURM_STEP_ID");
	const char *sharedir = getenv("SLURM_SUBMIT_DIR");
	if (!job_str)  job_str  = "local";
	if (!step_str) step_str = "0";
	if (!sharedir) sharedir = ".";
	char jobid_buf[64];
	snprintf(jobid_buf, sizeof(jobid_buf), "%s_%s", job_str, step_str);
	char path[512];
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%d", sharedir, jobid_buf, __node__);
	unlink(path);
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%d",  sharedir, jobid_buf, __node__);
	unlink(path);
#endif
}
/*
 * ---------------------------------------------------------------------
 * Setup variables environments for a block
 * ---------------------------------------------------------------------
 */
void cape_begin(unsigned char name_directive, long first, long second){
	PROF_START(_t_begin);
	switch(name_directive){
		case PARALLEL:
			open_parallel_window();
			break;
		case FOR:
			//ckpt_stop();
			__parallel_level__++;
			add_parallel_region(&__var_list_head, &__var_list_tail, __parallel_level__);
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);
			break;
		case FOR_NOWAIT:
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);
			break;
		case PARALLEL_FOR:
			open_parallel_window();
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);			
			break;
		case CRISTIAL:
			break;
		case SECTIONS_NOWAIT:
			__current_session__ = -1 ;
			break;
		case SECTIONS:
			__parallel_level__++;
			add_parallel_region(&__var_list_head, &__var_list_tail, __parallel_level__);
			__current_session__ = -1 ;
			break;
		default:
			break;
	}
	PROF_ACC(prof_cape_begin_ns, _t_begin);
	PROF_INC(prof_cape_begin_count);
#ifdef CAPE_PROFILE
	/* mark start of user compute window */
	prof_compute_start_ns = cape_get_ns();
	prof_compute_started = 1;
#endif
}

static int cape_sync_checkpoint()
{
	PROF_START(_t_sync);
	if (require_allreduce(EXIT_CHECKPOINT) < 0) {
		fprintf(stderr, "CAPE: allreduce checkpoint merge failed on node %d\n", __node__);
		return -1;
	}
	if (inject_checkpoint(after_ckpt, after_ckpt_size) < 0) {
		fprintf(stderr, "CAPE: checkpoint injection failed on node %d\n", __node__);
		return -1;
	}
	PROF_ACC(prof_sync_ckpt_ns, _t_sync);
	PROF_INC(prof_sync_count);
	return 0;
}
/*
 * ---------------------------------------------------------------------
 * Release variables environments of a block
 * ---------------------------------------------------------------------
*/
void cape_end(unsigned char name_directive, unsigned char ops_flag){
	PROF_START(_t_end);
#ifdef CAPE_PROFILE
	if (prof_compute_started) {
		prof_compute_ns += _t_end - prof_compute_start_ns;
		prof_compute_count++;
		prof_compute_started = 0;
	}
#endif
	switch(name_directive){
		case FOR:
			__time_stamp__ = __right__;			
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			remove_parellel_region(__parallel_level__);
			__parallel_level__ --;
			ckpt_start();
			break;
		case FOR_NOWAIT:
			break;
		case SECTIONS_NOWAIT:
			break;		
		case PARALLEL:
			__time_stamp__ = __pc__;
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			close_parallel_window();
			break;			
		case PARALLEL_FOR:	
			__time_stamp__ = __right__;
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			//print_data_in_checkpoint(after_ckpt, after_ckpt_size);
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			close_parallel_window();			
			break;
		case SECTIONS:
			__time_stamp__ = __pc__;
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			remove_parellel_region(__parallel_level__);
			__parallel_level__ --;
			ckpt_start();
			break;
		default:
			break;
	}
	PROF_ACC(prof_cape_end_ns, _t_end);
	PROF_INC(prof_cape_end_count);
}
/*
 * --------------------------------------------------------------------
 * Copy data into ckpt_data in FILE in memory
 * Structure of data {(addr, data), ....}
 * ---------------------------------------------------------------------
 */
int ckpt_start(){
	PROF_START(_t_ckpt_start);

	/* The snapshot is a flat concatenation of [addr:8][data] for each
	 * tracked variable. Compute the total size up-front, malloc once,
	 * then memcpy — bypassing open_memstream avoids routing 100s of MB
	 * through stdio's 4 KB buffer (thousands of cookie-write callbacks
	 * each doing a realloc + memcpy). For our use case that previously
	 * dominated ckpt_start. ckpt_data_stream is no longer needed; mark
	 * it NULL so ckpt_stop knows to skip fclose. */
	ckpt_data = NULL;
	ckpt_data_size = 0;
	ckpt_data_stream = NULL;

	if(__var_list_head == NULL) {
		PROF_ACC(prof_ckpt_start_ns, _t_ckpt_start);
		PROF_INC(prof_ckpt_start_count);
#ifdef CAPE_PROFILE
		prof_compute_start_ns = cape_get_ns();
		prof_compute_started = 1;
#endif
		return 0;
	}

	VarList *tmp;
	tmp = __var_list_tail;

	//move to the first variable of current parallel level
	while((tmp->var.level == __parallel_level__) && (tmp->prev != NULL))
		tmp = tmp->prev;

	VarList *first = tmp;
	size_t total = 0;
	while (tmp != NULL) {
		if ((tmp->var.pro != CAPE_PRIVATE) &&
		    (tmp->var.pro != CAPE_THREAD_PRIVATE))
			total += sizeof(unsigned long) +
			         (size_t)tmp->var.size * (size_t)tmp->var.n;
		tmp = tmp->next;
	}

	if (total == 0) {
		PROF_ACC(prof_ckpt_start_ns, _t_ckpt_start);
		PROF_INC(prof_ckpt_start_count);
#ifdef CAPE_PROFILE
		prof_compute_start_ns = cape_get_ns();
		prof_compute_started = 1;
#endif
		return 0;
	}

	ckpt_data = (unsigned char *)malloc(total);
	ckpt_data_size = total;

	size_t off = 0;
	tmp = first;
	while (tmp != NULL) {
		if ((tmp->var.pro != CAPE_PRIVATE) &&
		    (tmp->var.pro != CAPE_THREAD_PRIVATE)) {
			size_t bytes = (size_t)tmp->var.size * (size_t)tmp->var.n;
			memcpy(ckpt_data + off, &tmp->var.addr,
			       sizeof(unsigned long));
			off += sizeof(unsigned long);
			memcpy(ckpt_data + off,
			       (void*)(uintptr_t)tmp->var.addr, bytes);
			off += bytes;
		}
		tmp = tmp->next;
	}

	PROF_ACC(prof_ckpt_start_ns, _t_ckpt_start);
	PROF_INC(prof_ckpt_start_count);
#ifdef CAPE_PROFILE
	/* subsequent compute window (FOR loop repeat) begins now */
	prof_compute_start_ns = cape_get_ns();
	prof_compute_started = 1;
#endif
	return 0;
}
/*
 * --------------------------------------------------------------------
 * Copy data into ckpt_data in FILE in disk
 * Structure of data {(addr, data), ....}
 * ---------------------------------------------------------------------
 */
int ckpt_start_2(){
	if (__node__< 0) __node__ = 1;
	sprintf(__ckpt_data_file, "ckpt_data%d.tmp", __node__);	
	__ckpt_data = fopen(__ckpt_data_file, "wb+");
	__ckpt_data_size == 0;
		
	int size;
		
	if(__var_list_head == NULL) return 0;
	
	VarList *tmp;
	tmp = __var_list_tail;
	
	//move to the first variable of current parallel level
	while((tmp->var.level == __parallel_level__) && (tmp->prev != NULL))
		tmp = tmp->prev ;
	
	while(tmp != NULL){
		if ((tmp->var.pro != CAPE_PRIVATE) &&
			(tmp->var.pro != CAPE_THREAD_PRIVATE)){			
			fwrite(&tmp->var.addr, sizeof(unsigned long), 1, __ckpt_data);
			size = tmp->var.n * tmp->var.size ;
			fwrite((void*)(uintptr_t)tmp->var.addr, tmp->var.size, tmp->var.n, __ckpt_data);
			fflush(__ckpt_data);	
			__ckpt_data_size += sizeof(unsigned long) \							
								+ tmp->var.size * tmp->var.n ; 
			
			printf("Write to ckpt data file: Ox%lx - pro: %d  - size : %d \n", tmp->var.addr, tmp->var.pro, 
										sizeof(unsigned long)  + tmp->var.size * tmp->var.n);
		}
		
		tmp = tmp->next;
	}
	
	printf("Size of CKPT_DATA_FILE: %ld", __ckpt_data_size);
	
}

/*
 * ---------------------------------------------------------------------
 * TODO: close __ckpt_data file and syncronization data
 * ---------------------------------------------------------------------
 */
void ckpt_stop(){
	PROF_START(_t_stop);
	if (ckpt_data_size > 0){
		/* ckpt_start now allocates ckpt_data directly via malloc,
		 * so ckpt_data_stream is NULL — fclose only when present. */
		if (ckpt_data_stream) {
			fclose(ckpt_data_stream);
			ckpt_data_stream = NULL;
		}
		free(ckpt_data);
		ckpt_data = NULL;
		ckpt_data_size = 0;
	}
	PROF_ACC(prof_ckpt_stop_ns, _t_stop);
	PROF_INC(prof_ckpt_stop_count);
}

void ckpt_stop_2(){
	fclose(__ckpt_data);
}

/*
 * ---------------------------------------------------------------------
 * Set flush()
 * ---------------------------------------------------------------------
 */
void cape_flush(){		
	require_generate_checkpoint(FALSE);
	ckpt_stop();
	if (cape_sync_checkpoint() < 0) {
		release_checkpoint();
		exit(1);
	}
	release_checkpoint();
	ckpt_start();
}
/*
 * ---------------------------------------------------------------------
 * Set barrier
 * ---------------------------------------------------------------------
 */
void cape_barrier(){
	__time_stamp__  = __node__;
	cape_flush();
}
/* --------------------*/

void CAPE_DEBUG()
{	
	print_var_list(__active_variable_head);	
	printf("\n-----------\n");
	//print_var_list(__active_variable_head);	
	print_var_head_list(__var_heap_list_head);
	
	
}

/*
 * -------------------------------------------------------------------
 * Enter function: v7.0
 * 	Setup function variable of function in activation tree *  
 * --------------------------------------------------------------------
 */
void __enter_func(){
	PROF_START(_t_ef);
	__activate_func_level__ ++;
	PROF_ACC(prof_enter_exit_func_ns, _t_ef);
	PROF_INC(prof_enter_exit_count);
}
/*
 * -------------------------------------------------------------------
 * Exit function: v7.0
 * 	Remove parameters and local variables from shared variable list *  
 * --------------------------------------------------------------------
 */
void __exit_func(){
	PROF_START(_t_xf);
	remove_active_variables(&__active_variable_head,
							&__active_variable_tail,
							__activate_func_level__);
	__activate_func_level__ --;
	PROF_ACC(prof_enter_exit_func_ns, _t_xf);
	PROF_INC(prof_enter_exit_count);
}

void require_generate_checkpoint(char ops_flag){
	PROF_START(_t_gen);
	(void)generate_checkpoint(__var_list_head,
	                          __parallel_level__,
	                          EXIT_CHECKPOINT,
	                          ops_flag,
	                          __time_stamp__,
	                          __pc__);
	/* generate_checkpoint now returns NULL (manual buffer instead of
	 * stdio stream). after_ckpt is a plain malloc'd buffer ready for
	 * RDMA registration. */
	after_ckpt_stream = NULL;
	PROF_ACC(prof_generate_ckpt_ns, _t_gen);
	PROF_INC(prof_generate_ckpt_count);
}
