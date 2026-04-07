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
#include <poll.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <linux/userfaultfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

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

//list to manage incremental checkpoint of share-data attribute variables
struct shared_data_ckpt * list_ckpt_head = NULL;
struct shared_data_ckpt * list_ckpt_tail = NULL;

struct shared_data_ckpt * final_list_ckpt_head = NULL;
struct shared_data_ckpt * final_list_ckpt_tail = NULL;

int process_state = 0; //to follow the state of process
int child_id, parent_id;
int control_fd = -1;
int userfault_fd = -1;

struct cape_dickpt_range *tracked_ranges = NULL;
size_t tracked_range_count = 0;
int tracking_is_enabled = 0;

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

static FILE *open_binary_memstream(unsigned char **bufloc, size_t *sizeloc)
{
	return open_memstream((char **)bufloc, sizeloc);
}

static int read_remote_memory(pid_t pid, unsigned long remote_addr, void *local_buf,
			      size_t len)
{
	size_t done = 0;

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

	return 0;
}

static int write_remote_memory(pid_t pid, const void *local_buf,
			       unsigned long remote_addr, size_t len)
{
	size_t done = 0;

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

	return 0;
}

static int cape_userfault_writeprotect(unsigned long start, unsigned long len, int enable)
{
	struct uffdio_writeprotect wp;

	if (userfault_fd < 0)
		return -1;

	memset(&wp, 0, sizeof(wp));
	wp.range.start = start;
	wp.range.len = len;
	wp.mode = enable ? UFFDIO_WRITEPROTECT_MODE_WP : 0;

	return ioctl(userfault_fd, UFFDIO_WRITEPROTECT, &wp);
}

static int cape_capture_dirty_page(unsigned int pid, unsigned long fault_addr)
{
	unsigned long aligned_addr;
	struct page_node *temp_node, *current_node;

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
		return 0;
	}

	if (aligned_addr > list_end->addr) {
		current_node->before = list_end;
		list_end->next = current_node;
		list_end = current_node;
		return 0;
	}

	if (aligned_addr < list_head->addr) {
		current_node->next = list_head;
		list_head->before = current_node;
		list_head = current_node;
		return 0;
	}

	current_node->next = temp_node;
	current_node->before = temp_node->before;
	temp_node->before->next = current_node;
	temp_node->before = current_node;
	return 0;
}

static int cape_handle_userfault_event(void)
{
	struct uffd_msg msg;
	ssize_t nread;
	unsigned long page_addr;

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

	return 0;
}

int cape_drain_userfaultfd(void)
{
	struct pollfd pfd;

	if (userfault_fd < 0 || !tracking_is_enabled)
		return 0;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = userfault_fd;
	pfd.events = POLLIN;

	while (poll(&pfd, 1, 0) > 0) {
		if ((pfd.revents & POLLIN) == 0)
			break;
		if (cape_handle_userfault_event() != 0)
			return -1;
		pfd.revents = 0;
	}

	return 0;
}

int cape_wait_for_child_event(pid_t pid, int *status)
{
	if (userfault_fd < 0 || !tracking_is_enabled)
		return waitpid(pid, status, 0);

	for (;;) {
		pid_t rc = waitpid(pid, status, WNOHANG);

		if (rc == pid)
			return rc;
		if (rc == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (cape_drain_userfaultfd() != 0)
			return -1;

		{
			struct pollfd pfd;
			int poll_rc;

			memset(&pfd, 0, sizeof(pfd));
			pfd.fd = userfault_fd;
			pfd.events = POLLIN;
			poll_rc = poll(&pfd, 1, 50);
			if (poll_rc == -1 && errno != EINTR)
				return -1;
			if (poll_rc > 0 && (pfd.revents & POLLIN) != 0) {
				if (cape_handle_userfault_event() != 0)
					return -1;
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

/* Block until a non-blocking UCX request finishes. */
static void cape_ucx_wait(void *req, size_t expect_len, int check_len,
                          ucp_tag_t *out_tag)
{
    if (out_tag) *out_tag = 0;
    if (req == NULL)
        return;
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
}

/* Simple blocking send via UCX tag. */
static void cape_ucx_send(const void *buf, size_t len, int dest, uint32_t token)
{
    ucp_tag_t tag = CAPE_UCX_TAG(node, token);
    ucp_request_param_t sp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = cape_send_cb
    };
    void *req = ucp_tag_send_nbx(ucp_endpoints[dest], buf, len, tag, &sp);
    cape_ucx_wait(req, 0, 0, NULL);
}

/* Simple blocking recv via UCX tag. */
static void cape_ucx_recv(void *buf, size_t len, int src, uint32_t token)
{
    ucp_tag_t tag = CAPE_UCX_TAG(src, token);
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                      | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };
    void *req = ucp_tag_recv_nbx(ucp_worker, buf, len, tag, CAPE_UCX_TAG_MASK, &rp);
    cape_ucx_wait(req, len, 1, NULL);
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
    const char *jobid = getenv("CAPE_UCX_BOOTSTRAP_ID");
    if (!jobid || !*jobid)
        jobid = getenv("SLURM_JOB_ID");
    if (!jobid || !*jobid)
        jobid = "local";
    return jobid;
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
/* Exchange addresses via shared filesystem (same approach as cape.c) */
static void ucx_exchange_addresses_via_fs(const char *dir, const char *jobid,
                                          ucp_address_t *local_addr,
                                          size_t local_addr_len)
{
    char path[512];
    FILE *f;

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

    snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%ld", dir, jobid, node);
    f = fopen(path, "w");
    if (!f) { perror("CAPE UCX: write rdy file"); exit(1); }
    fclose(f);

    for (int i = 0; i < num_nodes; i++) {
        snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%d", dir, jobid, i);
        while (access(path, F_OK) != 0)
            usleep(10000);
    }

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
    PMIx_Commit();
    PMIx_Fence(&wildcard, 1, NULL, 0);

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
        printf("Monitor %ld: closing ep %d\n", node, i);
        fflush(stdout);
        void *req = ucp_ep_close_nbx(ucp_endpoints[i], &close_param);
        if (UCS_PTR_IS_ERR(req)) {
            printf("Monitor %ld: ep %d close error\n", node, i);
            fflush(stdout);
            continue;
        }
        if (req == NULL) {
            printf("Monitor %ld: ep %d closed immediately\n", node, i);
            fflush(stdout);
            continue;
        }
        printf("Monitor %ld: ep %d waiting for close\n", node, i);
        fflush(stdout);
        while (ucp_request_check_status(req) == UCS_INPROGRESS)
            ucp_worker_progress(ucp_worker);
        ucp_request_free(req);
        printf("Monitor %ld: ep %d closed\n", node, i);
        fflush(stdout);
    }
    free(ucp_endpoints);
    ucp_endpoints = NULL;

    printf("Monitor %ld: destroying worker\n", node);
    fflush(stdout);
    ucp_worker_destroy(ucp_worker);
    printf("Monitor %ld: cleaning up context\n", node);
    fflush(stdout);
    ucp_cleanup(ucp_context);

#ifdef USE_PMIX
    PMIx_Finalize(NULL, 0);
#else
    const char *jobid    = cape_ucx_bootstrap_id();
    const char *sharedir = cape_ucx_bootstrap_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%ld", sharedir, jobid, node);
    unlink(path);
    snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%ld",  sharedir, jobid, node);
    unlink(path);
#endif
}

/* ---------------------------------------------------------------------------
 * Prototypes
 * ---------------------------------------------------------------------------
 */
int clear_list(struct page_node *list);
int send_int_value_to_child(int value);
int get_long_int_from_child(unsigned long *value);
int init_jobs_per_node();
int read_current_stack_start(unsigned int pid, unsigned long src, unsigned long dst, int len);
int read_current_brk(unsigned int pid, unsigned long src, unsigned long dst, int len);
int ioctl_read_data(unsigned int pid, unsigned long src, void *dst, int len);
int ioctl_write_data(unsigned int pid, const void *src, unsigned long dst, int len);
int ioctl_clear_write_protect(unsigned int pid, unsigned long dst);
void tracer_wait ( pid_t pid, int * status, int options, struct user * u );
int lock_process_memory(unsigned int pid);
int unlock_process_memory(unsigned int pid);
int require_generate_checkpoint();
int require_send_checkpoint();
int require_receive_checkpoint();
int require_inject_checkpoint();
int require_merge_checkpoint();
int require_waitfor_checkpoint();
int require_broadcast_checkpoint();
int read_shared_data();
void end_shared_data();

int init_generate_workshare_checkpoint(unsigned int ntask);
int require_generate_workshare_checkpoint();
int require_generate_total_checkpoint(); //generate total checkpoint and broadcast to all nodes
int require_scatter_checkpoint();
int require_inject_workshare_checkpoint();

int require_allreduce_checkpoint();
int allreduce_checkpoint();
void print_data_in_list(struct shared_data *list);
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
	int addr, i, state, status, old, sys_num, rc;
	struct user u ;
	siginfo_t child_siginfo;
	struct sigaction sa;
	int tc = 0, flag_192 = 0, main_flag = 0, opt, reuse = 1;
	struct user_regs_struct regs, newregs;
	int control_pair[2];
	char control_fd_text[32];

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <app>\n", argv[0]);
		return 1;
	}
	
	exec_file = argv[1];

	cape_ucx_init();

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
			ptrace ( PTRACE_TRACEME, NULL, NULL, NULL ) ; //This process will be traced
			execl ( exec_file , exec_file , NULL);			
			perror (exec_file) ;
			return 2 ;
		default :	/* Parent */
			control_fd = control_pair[0];
			close(control_pair[1]);
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

	ptrace(PTRACE_SYSCALL, child_id, NULL, NULL ) ;						
	
	//Monitor CAPE program
	while(1) {
		if (cape_wait_for_child_event(child_id, &status) == -1) {
			perror("waitpid");
			break;
		}
		if(WIFEXITED(status)) {
			printf("Monitor %ld: child exited with code %d\n", node, WEXITSTATUS(status));
			fflush(stdout);
			break;
		}
		if (( sys_num = ptrace(PTRACE_PEEKUSER, child_id, 8 * ORIG_RAX, NULL) ) == -1 ) { //catch a signal
			if(ptrace(PTRACE_GETSIGINFO, child_id, NULL, &child_siginfo)){
				perror("PTRACE_GETSIGINFO");
				break;
			}
			if(child_siginfo.si_signo == SIGTRAP){  //SIGTRAP: this is Debuging signal 
				int dx = 0;
				ptrace(PTRACE_GETREGS, child_id, NULL, &regs);
				dprintf("Monitor %ld: Signal SIGTRAP caught, edx = 0x%lx, child-id = %d \n", 
					node, regs.rdx, child_id);		
				dx = regs.rdx;
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
						
					case S_GENERATE_TOTAL_CHECKPOINT:
						rc = require_generate_total_checkpoint();
						break;	
						
					case S_GENERATE_WORKSHARE_CHECKPOINT:
						rc = require_generate_workshare_checkpoint();
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
						
					case S_INJECT_WORKSHARE_CHECKPOINT:
						rc = require_inject_workshare_checkpoint();						
						break;
					
					case S_MERGE_CHECKPOINT:
						rc= require_merge_checkpoint();
						if( rc !=0) exit(1);						
						break;
					case S_WAIT_FOR_CHECKPOINT:
						rc= require_waitfor_checkpoint();
						if( rc !=0) exit(1);						
						break;
					case S_BROADCAST_CHECKPOINT:
						rc= require_broadcast_checkpoint();
						if( rc !=0) exit(1);						
						break;

					case S_SCATTER_CHECKPOINT:
						rc = require_scatter_checkpoint();
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
						init_generate_workshare_checkpoint(number_of_jobs);
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
			
			if(child_siginfo.si_signo == SIGSEGV){
				fprintf(stderr,
					"Monitor %ld: child received SIGSEGV at 0x%lx\n",
					node, (unsigned long)child_siginfo.si_addr);
				kill(child_id, SIGKILL);
				return 1;
			}//SIGSEGV
		}
		else{  //a system call
			dprintf(" \nMonitor %ld: System call number %d \n", node, sys_num ) ;
			if(sys_num == 252){ //child finish
				printf("\nMonitor %ld: child finish\n", node);
				break;
			}
		}			
		if(sys_num == 45  && process_state == 3){				
				dprintf("Monitor %ld: Signal 45, process_state = %d\n", node, process_state);
		}
		ptrace(PTRACE_SYSCALL, child_id, NULL, NULL ) ;
	}
	printf("Monitor %ld: exited main loop, status=0x%x\n", node, status);
	fflush(stdout);
	if (userfault_fd >= 0)
		close(userfault_fd);
	if (control_fd >= 0)
		close(control_fd);
	free(tracked_ranges);
	printf("Monitor %ld: calling cape_ucx_finalize\n", node);
	fflush(stdout);
	cape_ucx_finalize();
	printf("Monitor %ld: parent finish!\n", node);
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
 	if(number_of_jobs % num_nodes == 0)
		jobs_per_node = number_of_jobs/num_nodes;
	else
		rc = 1;
	current_job = 0;
	return rc;	
	
 }
/* ---------------------------------------------------
 * read_current_stack_start(): read the start adress of stack 
 * ---------------------------------------------------
 */
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
/* ---------------------------------------------------
 * read_current_brk(): read the top address of heap
 * ---------------------------------------------------
 */
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
 int ioctl_clear_write_protect(unsigned int pid, unsigned long dst){
	if (cape_capture_dirty_page(pid, dst) != 0)
		return 1;
	if (cape_userfault_writeprotect(dst & ~(PAGE_SIZE - 1), PAGE_SIZE, 0) == -1)
		return 1;
	return 0;
}

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

/* ---------------------------------------------------------------------
 * check_addr_in_data_list(): check addr that contains in shared_data or not
 * Some cases:
 * 		1) (addr>=node->addr) and (addr+len <= node->addr + node->len
 * 		2) (addr >= node-> addr ) and (addr < node->addr + node->len)
 * 		3) (addr < node->addr)  and (addr + len > node-> addr)
 * 		4) (addr < node-> addr) and (addr + len > node->addr + node->len)
 * 	return: 
 * 		point to node of list if that is found
 * 		NULL: if it is not found
 * ---------------------------------------------------------------------
 */
/*
struct shared_data * check_addr_in_data_list(long int addr, int len,  struct shared_data *dp, int *flag){
	struct shared_data *p;	
	p = dp;		
	while(list!= NULL){		
		if(addr >= p->addr){
			if (addr+len <= p->addr + p->len){ 
				*flag = 1;				
				return p;
			}
			else{}
				
		}
				
			
		list = list -> next;
	}
	
	
	return NULL;
}

*/

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
	struct shared_data *ps = NULL;
	unsigned long int addr2;
	ps = list;
	while(ps!=NULL){
		addr2 = ps->addr;
		while(addr2 < (ps->addr + ps->len)){
			if ( addr == addr2 ){
				return ps;
			}
			addr2 = addr2 + CAPE_WORD;
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
	pt->addr = p->addr ;
	memcpy(pt->data, p->data, CAPE_WORD);
	
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
//	struct shared_data *dp;
//	int read_size = sizeof(struct shared_data);
	struct shared_data_ckpt *p;
	struct shared_data *plist = NULL;
	
	int rc, i, count, len, num_read_pages = 0;
	struct page_node * old_node, * current_node;
	unsigned long addr, start, offset = 0, ckpt_len = 0, _timespan;
	int * buff[PAGE_SIZE_DIV_4];
	int shared_flag = 0;
	unsigned long current_ckpt_struct, new_ckpt_struct;
	current_ckpt_struct = NA; //Init current checkpoint structure
	
	
	//test print data in kernel
	//rc = check_addr_in_data_list(addr, data_list_head);
	
	//print_data_in_list(data_list_head);
	
	//open the stream memory file
	stream = open_binary_memstream(ckpt_data, ckpt_size);		
		
	//get the registers
	ptrace(PTRACE_GETREGS, child_id, NULL, &save_regs);
	
	//write timespan into checkpoint
	_timespan = tsp;
	fwrite(&_timespan, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	//save register to file
	fwrite(&save_regs,sizeof(struct user_regs_struct),1, stream);
	fflush(stream);
	
	if (list == NULL) return stream;

	//find the different data and save to checkpoint file
	old_node = list;
	current_node = malloc(sizeof(struct page_node));
	
	while(old_node != NULL){				
		//read the current data of modified page and save to current node
		rc = ioctl_read_data(child_id, old_node->addr,
						&(current_node->data),
						 PAGE_SIZE);		
						 		
		//Read and compare 4 bytes of data in a time
		offset = 0;		
		while(offset < PAGE_SIZE_DIV_4){
			//search the first different byte between the saved page and the current page
			while((offset < PAGE_SIZE_DIV_4) && (old_node->data[offset] == current_node->data[offset]))
				offset++ ;

			if(offset == PAGE_SIZE_DIV_4) break;			
			//search the different string from the offset
			start = offset;
			plist = NULL;
			//shared_flag = 0;
			while((offset < PAGE_SIZE_DIV_4) && (old_node->data[offset] != current_node->data[offset])){
				plist = is_in_share_data_list(old_node->addr + (offset * CAPE_WORD), data_list_head);						
				if (plist != NULL){
					//shared_flag = 1 ;
					//printf("\nNode %ld: 0x%lx is IN list \n", node, old_node->addr + (offset * 4));
					break;
				}					
				offset++ ;				
			}				
			//Save SD1 part of incremental checkpoint - Shared data by default
			if(offset > start){
				len = (offset - start) * CAPE_WORD; //len in bytes 
				addr = (old_node->addr + (start * CAPE_WORD)); //begin address of block data have been change			
				
				//printf("\nNode %ld: ENTER S1 PART -  0x%lx is IN list - len = %d \n", node, old_node->addr + (offset * 4), len);
				
				//setup new checkpoint struct that depends on len of data
				if(len == PAGE_SIZE)
					new_ckpt_struct = EP;
				else if (len > CAPE_WORD) 
						new_ckpt_struct = SSD; 
					 else
						new_ckpt_struct = SD;						
				//Write ckpt_struct into ckpt file if it is changed
				if (current_ckpt_struct != new_ckpt_struct){
					current_ckpt_struct = new_ckpt_struct;
					fseek(stream, *ckpt_size,SEEK_SET);
					fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, stream);
					fflush(stream);									
				}				
				//save data into checkpoint file; depends on current_ckpt_struct
				switch(current_ckpt_struct){
					case SSD:
						fseek(stream, *ckpt_size,SEEK_SET);
						//save address to file
						fwrite(&addr,sizeof(unsigned long),1, stream);						
						//save len to file
						fwrite(&len, sizeof(int), 1, stream);							
						//save data to file
						fwrite(&(current_node->data[start]), len, 1,stream);
						fflush(stream);						
						break;
					case SD:
						fseek(stream, *ckpt_size,SEEK_SET);
						//save address to file
						fwrite(&addr,sizeof(unsigned long),1, stream);
						//save data to file
						fwrite(&(current_node->data[start]), len, 1,stream);
						fflush(stream);	
						break;
					case EP:
						fseek(stream, *ckpt_size,SEEK_SET);
						//save address to file
						fwrite(&addr,sizeof(unsigned long),1, stream);						
						//save data to file
						fwrite(&(current_node->data[start]), len, 1,stream);
						fflush(stream);											
						break;									
						
				}
								
			}//end SD1
			
			
			//Search for SD2 part - Data in SHARED clause
			start = offset;
			while((offset < PAGE_SIZE_DIV_4) && 
					(old_node->data[offset] != current_node->data[offset])
					&& plist != NULL)
			{				
				plist = is_in_share_data_list(old_node->addr + (offset * CAPE_WORD), data_list_head);						
				if (plist->properties != D_SHARED){
					//shared_flag = 1 ;					
					break;
				}
				//printf("\nNode %ld: 0x%lx is IN list - SHARED", node, old_node->addr + (offset * 4));					
				offset++ ;
			}				
			//If offset is in SHARED list
			if (offset > start) {
				len = (offset - start) * CAPE_WORD; //len in bytes 
				addr = (old_node->addr + (start * CAPE_WORD)); //begin address of block data have been change
				//setup new checkpoint struct that depends on len of data
				if(len == PAGE_SIZE)
					new_ckpt_struct = EP;
				else if (len > CAPE_WORD) 
						new_ckpt_struct = SSD; 
					 else
						new_ckpt_struct = SD;
				//Write ckpt_struct into ckpt file if it is changed
				if (current_ckpt_struct != new_ckpt_struct){
					current_ckpt_struct = new_ckpt_struct;
					fseek(stream, *ckpt_size,SEEK_SET);
					fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, stream);
					fflush(stream);									
				}
				//save data into checkpoint file; depends on current_ckpt_struct
				switch(current_ckpt_struct){
					case SSD:
						fseek(stream, *ckpt_size,SEEK_SET);
						//save address to file
						fwrite(&addr,sizeof(unsigned long),1, stream);						
						//save len to file
						fwrite(&len, sizeof(int), 1, stream);							
						//save data to file
						fwrite(&(current_node->data[start]), len, 1,stream);
						fflush(stream);						
						break;
					case SD:
						fseek(stream, *ckpt_size,SEEK_SET);
						//save address to file
						fwrite(&addr,sizeof(unsigned long),1, stream);
						//save data to file
						fwrite(&(current_node->data[start]), len, 1,stream);
						fflush(stream);	
						break;
					case EP:
						fseek(stream, *ckpt_size,SEEK_SET);
						//save address to file
						fwrite(&addr,sizeof(unsigned long),1, stream);						
						//save data to file
						fwrite(&(current_node->data[start]), len, 1,stream);
						fflush(stream);											
						break;									
						
				}			
			}//END SD2
			
			
						
			//copy memory at shared list and save in list L	
			//NOTE: Check the properties of each node in list....	
			if((plist != NULL) && (plist->properties != D_SHARED))
			{
				if(cflag == ENTRY_CHECKPOINT){
					if ((plist->properties != D_THEAD_PRIVATE) &&
							(plist->properties != D_PRIVATE) &&
							(plist->properties !=D_LAST_PRIVATE)){
								
						p = malloc(sizeof(struct shared_data_ckpt));
						p->addr = old_node->addr + (offset * CAPE_WORD);				
						memcpy(p->data, current_node->data + offset, CAPE_WORD);	
						
//float f= 0.0;
//memcpy(&f, current_node->data + offset, CAPE_WORD);	
//printf("\nNode %ld: 0x%lx is SENT TO ADD to list ckpt - value = %f", node, old_node->addr + (offset * 4), f);	
						
						
						add_item_to_list_ckpt(p);
						free(p);
																
					}												
				}
				else{ // EXIT_CHECKPOINT
					if ((plist->properties != D_THEAD_PRIVATE) &&
							(plist->properties != D_PRIVATE) &&
							 (plist->properties != D_COPY_IN) &&
							(plist->properties !=D_FIRST_PRIVATE)){	
															
						p = malloc(sizeof(struct shared_data_ckpt));
						p->addr = old_node->addr + (offset * CAPE_WORD);				
						memcpy(p->data, current_node->data + offset, CAPE_WORD);
						
						//float f= 0.0;
						//memcpy(&f, current_node->data + offset, CAPE_WORD);	
						//printf("\nNode %ld: 0x%lx is SENT TO ADD to list ckpt - value = %f", node, old_node->addr + (offset * 4), f);	
						add_item_to_list_ckpt(p);
						free(p);
																	
					} 
				}
				offset++;					
			}
		
		}
		//set write protect to the page
		rc = ioctl_set_write_protect(child_id, old_node->addr);
		old_node = old_node->next;	
		
	}	
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
	
	printf("Monitor %ld - generated %zu bytes checkpoint \n", node, final_ckpt_size);
	
	clear_list(list_head);
	list_head = NULL;	
	
	print_data_in_ckpt_list(list_ckpt_head);
	
	if(rc!=0) printf("Monitor %ld: Error on requiring generate checkpoint\n", node);	
	return rc;
}

/* ----------------------------------------------------------------------
 * require_generate_total_checkpoint(): Version 4.0 -> Not use in version 5
 * 	1. generate checkpoint and save into a memory file stream
 * 	2. re-lock pages
 * 	3. clear list
 * After generate ckpt, bcast to all nodes
 * -----------------------------------------------------------------------
 */
int require_generate_total_checkpoint(){
	int rc=0;

	rc = cape_drain_userfaultfd();
	if (rc != 0)
		return rc;

	//generate new check point
	total_ckpt_stream = generate_checkpoint(child_id,
											list_head,
											&total_ckpt,
											&total_ckpt_size,
											ENTRY_CHECKPOINT,
											timespan);
	//printf("Monitor %ld - generated %d bytes checkpoint \n", node, after_ckpt_size);
	clear_list(list_head);	
	
	if(rc!=0) printf("Monitor %ld: Error on requiring generate checkpoint\n", node);	
	return rc;
}


/*----------------------------------------------------------------------
 * init_generate_workshare_checkpoint(): V4.0 - Not use in version 5
 * 	init data that will be used in require_generate_workshare_checkpoint() 
 * ---------------------------------------------------------------------
 */
int init_generate_workshare_checkpoint(unsigned int ntask){
	int rc=0;
	
	mbefore_ckpt_stream = open_binary_memstream(&mbefore_ckpt, &mbefore_ckpt_size);	
	
	return rc;

}

/* ----------------------------------------------------------------------
 * require_generate_workshare_checkpoint(): Version 4.0 - NOT use in version 5
 * This functions will be called by master only
 * 	1. generate checkpoint and save into array of memory stream
 * 	2. re-lock pages
 * 	3. clear list
 * Results: will be saved into mbefore_ckpt_streams
 * -----------------------------------------------------------------------
 */
int require_generate_workshare_checkpoint(){
	int rc=0;
	unsigned char *before_ckpt;
	FILE *before_ckpt_stream;
	size_t before_ckpt_size;
	int ckpt_struct = SD;
	struct shared_data_ckpt *p;

	
	if(node==0){		
		rc = cape_drain_userfaultfd();
		if (rc != 0)
			return rc;

		//generate checkpoints
		before_ckpt_stream =  generate_checkpoint(child_id,
			list_head, 
			&before_ckpt, 
			&before_ckpt_size,
			ENTRY_CHECKPOINT, timespan);			
		//printf("Monitor %ld - generated %d bytes checkpoint \n", node, before_ckpt_size);		
		clear_list(list_head);		
		
		
		//add L part into checkpoint, if it exist
		if(list_ckpt_head != NULL){	
												//print_data_in_ckpt_list(list_ckpt_head);
															
				fseek(before_ckpt_stream, before_ckpt_size, SEEK_SET);
				//Write signal of data structure 	
				fwrite(&ckpt_struct, sizeof(unsigned long), 1, before_ckpt_stream);
				fflush(before_ckpt_stream);
				//read all node in list and save into checkpoint file
				p = list_ckpt_head;
				while(p!=NULL){
					fwrite(&p->addr, sizeof(unsigned long), 1, before_ckpt_stream);				 
					fwrite(p->data, CAPE_WORD, 1, before_ckpt_stream);
					fflush(before_ckpt_stream);		 
					p = p->next;		 
				}	
			list_ckpt_head = NULL;
			list_ckpt_tail = NULL;
		}

		//add a workshare checkpoint into group of workshared checkpoints
		fwrite(before_ckpt, sizeof(char), before_ckpt_size, mbefore_ckpt_stream);		
		fflush(mbefore_ckpt_stream);
		
		//close before_ckpt_stream	
		fclose(before_ckpt_stream);
		before_ckpt_size = 0;		
	}	
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
int send_checkpoint(int destination){
	/* First send the size so the receiver knows how much to allocate */
	int sz = (int)final_ckpt_size;
	cape_ucx_send(&sz, sizeof(int), destination, TAG_CKPT_DATA);
	/* Then send the checkpoint data */
	cape_ucx_send(final_ckpt, final_ckpt_size, destination, TAG_CKPT_DATA + 1);

	fclose(final_ckpt_stream);
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
			   //Open checkpoint file if it is closed
				if (final_ckpt_size == 0){
					final_ckpt_stream = open_binary_memstream(&final_ckpt, &final_ckpt_size);
				}
				else{
					fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				}
				//Write signal of data structure 	
				fwrite(&ckpt_struct, sizeof(unsigned long), 1, final_ckpt_stream);
				fflush(final_ckpt_stream); 
				//read all node in list and save into checkpoint file
				while(p1!=NULL){
					fwrite(&p1->addr, sizeof(unsigned long), 1, final_ckpt_stream);
					fflush(final_ckpt_stream);		 
					fwrite(p1->data, CAPE_WORD, 1, final_ckpt_stream);
					fflush(final_ckpt_stream);		 
					p1 = p1->next;		 
				}			
			break;
			
		case TOTAL_CHECKPOINT:
				fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
				//Write signal of data structure 	
				fwrite(&ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
				fflush(total_ckpt_stream); 
				//read all node in list and save into checkpoint file
				while(p1!=NULL){
					fwrite(&p1->addr, sizeof(unsigned long), 1, total_ckpt_stream);
					fflush(total_ckpt_stream);		 
					fwrite(p1->data, CAPE_WORD, 1, total_ckpt_stream);
					fflush(total_ckpt_stream);		 
					p1 = p1->next;		 
				}			
				
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
	//Merge S and L, and then delete L form memory
//	print_data_in_ckpt_list(list_ckpt_head);
	
	join_checkpoint(FINAL_CHECKPOINT, list_ckpt_head);
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
			case SSD:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = SSD;
				
				//printf("\nNode %ld: addr = %lx - current_ckpt_struct = SSD", node, addr);			
				break;
			case SD:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = SD;	
				
				printf("\nInject: Node %ld: addr = %lx - current_ckpt_struct = SD", node, addr);
				break;
			case EP:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = EP;	
				//printf("\nNode %ld: addr = %ld - current_ckpt_struct = EP", node, addr);
				break;
			case MD:
				fread(&addr, sizeof(unsigned long), 1, stream);
				file_pointer += sizeof(unsigned long);
				fseek(stream, file_pointer, SEEK_SET);				
				current_ckpt_struct = MD;
				//printf("\nNode %ld: addr = %lx - current_ckpt_struct = MD", node, addr);	
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
		//printf("\nNode %ld: addr = %lx - current_ckpt_struct = %d - len = %d", node, addr, current_ckpt_struct, len);	 		
  		//read data from checkpoint
  		buff = (unsigned char *) malloc(len);
  		fread(buff, len, 1, stream);
  		file_pointer +=len;
  		fseek(stream, file_pointer, SEEK_SET);  		
 		
 		//printf("\nInject: Node %ld: current_file_pointer = %d - file size = %d", node, file_pointer, *file_size);	 		
 		
  		//write data to memory  		
  		ioctl_write_data(child_id, buff, addr, len);  		
  	}  	
  	
  	fclose(stream); 
  	*file_size = 0;
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
  	
  	printf("\nNode %ld: After Synchronized: TOTAL CHECKPOINT SIZE = %zu", node, total_ckpt_size);
  	
  	//Inject checkpoint	
	rc = inject_checkpoint(total_ckpt_stream, &total_ckpt_size, &save_regs); 	
  	 
  	
  	return rc;
  }
/* -----------------------------------------------------------------
  * require_inject_workshare_checkpoint(): v4.0 - NOT USE IN VERSION 5
  * 	inject workshare checkpoint into CAPE program memory
  * 	get checkpoint form beffor_buffer, then copy into a file and call inject_checkpoint function
  * ----------------------------------------------------------------
  */
int require_inject_workshare_checkpoint(){
	int rc =0;
	int i;
	unsigned char *before_ckpt;
	FILE *before_ckpt_stream;
	size_t before_ckpt_size;	
	
	if (before_buffer == NULL) return 1; //checkpoint is empty
	if (task_ckpt_size == 0 ) return 1; //checkpoint is empty
	
	if (current_job >= jobs_per_node) return 1;
	
	before_ckpt_stream = open_binary_memstream(&before_ckpt, &before_ckpt_size);
		
	fwrite(before_buffer + (current_job * task_ckpt_size),\
		   sizeof(char), 		\
		   task_ckpt_size,    \
		   before_ckpt_stream);
		   
	fflush(before_ckpt_stream);
			
	inject_checkpoint(before_ckpt_stream, &before_ckpt_size, &save_regs);			
	
	current_job ++;	
	
	return rc;
}



/* -------------------------------------
  * merge_checkpoint():  NOT USE IN VERSION 5
  * Merge checkpoint after in checkpoint final at one node
  * 	After merging: close after_ckpt
  *
  * C1 = S1 + L1
  * C2 = S2 + L2
  * This function just merge S1 vs S2, L1 vs L2 is merged automatically in generate_checkpoint function.
  *  -------------------------------------
  */
 int merge_checkpoint() {
 	unsigned char *buff;
 	unsigned long addr;
 	unsigned long file_pointer =  0;
 	int len, rc =0; 
 	unsigned long current_ckpt_struct, new_ckpt_struct;
 	current_ckpt_struct = SSD; //init checkpoint structure
 	
 	//IF final_ckpt = NULL
 	if(final_ckpt_size==0)
 	{
 		final_ckpt_stream = open_binary_memstream(&final_ckpt, &final_ckpt_size);
 		//get the registers
		ptrace(PTRACE_GETREGS, child_id, NULL, &save_regs);
	
		//save register to file
		fwrite(&save_regs,sizeof(struct user_regs_struct),1, final_ckpt_stream);
		fflush(final_ckpt_stream);
 	}
 	
 	
 	file_pointer += sizeof(struct user_regs_struct);
 	fseek(after_ckpt_stream, file_pointer, SEEK_SET);

 	while(file_pointer < after_ckpt_size)
 	{
 		
 		//read address from after checkpoint
  		fread(&addr, sizeof(unsigned long), 1, after_ckpt_stream);
  		file_pointer += sizeof(unsigned long);
  		fseek(after_ckpt_stream, file_pointer, SEEK_SET);
  		
  		//if addr is the struct signal, not an adress, then we read address again, and write signal into final_ckpt
  		switch(addr){
			case EP:
				fread(&addr, sizeof(unsigned long), 1, after_ckpt_stream);
				file_pointer += sizeof(unsigned long);
				fseek(after_ckpt_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = EP;
				
				//write signal into final checkpoint
				fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, final_ckpt_stream);
				fflush(final_ckpt_stream);
				
				break;
			case SSD:
				fread(&addr, sizeof(unsigned long), 1, after_ckpt_stream);
				file_pointer += sizeof(unsigned long);
				fseek(after_ckpt_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = SSD;
				
				//write signal into final checkpoint
				fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, final_ckpt_stream);
				fflush(final_ckpt_stream);
				
				break;
			case SD:
				fread(&addr, sizeof(unsigned long), 1, after_ckpt_stream);
				file_pointer += sizeof(unsigned long);
				fseek(after_ckpt_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = SD;
				//write signal into final checkpoint
				fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, final_ckpt_stream);
				fflush(final_ckpt_stream);
				
				break;
			case MD:
				fread(&addr, sizeof(unsigned long), 1, after_ckpt_stream);
				file_pointer += sizeof(unsigned long);
				fseek(after_ckpt_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = MD;
				//write signal into final checkpoint
				fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, final_ckpt_stream);
				fflush(final_ckpt_stream);				
				break;
		}
  		
  		//read data and write to final checkpoint
  		switch(current_ckpt_struct){
			case EP:
				len = PAGE_SIZE;
				//read data from checkpoint
				buff = (unsigned char *) malloc(len);
				fread(buff, len, 1,after_ckpt_stream);
				file_pointer +=len;
				fseek(after_ckpt_stream, file_pointer, SEEK_SET); 
				
				//write into final checkpoint
				fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				fwrite(&addr, sizeof(unsigned long), 1, final_ckpt_stream); 		
				fwrite(buff, len, 1, final_ckpt_stream);
				fflush(final_ckpt_stream); 				
				break;
			case SSD:
				  //read len from checkpoint
				len = 0;
				fread(&len, sizeof(int), 1, after_ckpt_stream);
				file_pointer += sizeof(int);
				fseek(after_ckpt_stream, file_pointer, SEEK_SET);
				
				 //read data from checkpoint
				buff = (unsigned char *) malloc(len);
				fread(buff, len, 1,after_ckpt_stream);
				file_pointer +=len;
				fseek(after_ckpt_stream, file_pointer, SEEK_SET); 
  		
				//write into final checkpoint
				fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				fwrite(&addr, sizeof(unsigned long), 1, final_ckpt_stream);
				//fflush(final_ckpt_stream);
				fwrite(&len, sizeof(int), 1, final_ckpt_stream);
				//fflush(final_ckpt_stream);
				fwrite(buff, len, 1, final_ckpt_stream);
				fflush(final_ckpt_stream); 				
				break;
			case SD:
				len = CAPE_WORD;
				//read data from checkpoint
				buff = (unsigned char *) malloc(len);
				fread(buff, len, 1,after_ckpt_stream);
				file_pointer +=len;
				fseek(after_ckpt_stream, file_pointer, SEEK_SET); 
				
				//write into final checkpoint
				fseek(final_ckpt_stream, final_ckpt_size,SEEK_SET);
				fwrite(&addr, sizeof(unsigned long), 1, final_ckpt_stream); 		
				fwrite(buff, len, 1, final_ckpt_stream);
				fflush(final_ckpt_stream); 				
				break;
			case MD:
				break;				
		}   			
 	}
 	
 	//printf("\n Node %ld: Merged %d bytes in to final ckpt: Final ckpt = %d", node, after_ckpt_size, final_ckpt_size);
 	fclose(after_ckpt_stream);
 	after_ckpt_size = 0;	  		 	 	
 	return rc; 	
 }


/*----------------------------------------------------------------
 * require_merge_checkpoint(): Version 3.0 => NOT USE in V4.0
 * 		call merge_checkpoint function
 * ---------------------------------------------------------------
 */
 int require_merge_checkpoint() {
 	int rc = 0;
 	
 	rc = merge_checkpoint();
 	
 	printf("Monitor %ld: After Merged checkpoint: final_ckpt = %zu - after_ckpt = %zu\n",
 		node, final_ckpt_size, after_ckpt_size);
 	
 	if (rc!=0) dprintf ("Monitor: Error on locking the process image\n");
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
	struct shared_data *ppt; //properties
	
	pt = malloc(sizeof(struct shared_data_ckpt));
	pt->addr = plist->addr ;
	memcpy(pt->data, plist->data, CAPE_WORD);
	
	printf("\nNode %ld: addr = %lx will be add into list_final_ckpt ", node, pt->addr );
	
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
printf("\n ---------- final_list_ckpt: Node %ld - addr = %lx  - Sum = %f + %f = %f", node, tmp->addr, f1, f2, f); 
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
printf("\n EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEee");
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
	unsigned char *buff;
 	unsigned long addr;
 	int len;
	
	current_ckpt_struct = SSD; //initial checkpoint struct
	
	file_pointer += s_position;
 	fseek(s_stream, (long)file_pointer, SEEK_SET);
 	
 	while(file_pointer < s_size)
 	{
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
				fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
				fflush(total_ckpt_stream);
				
				break;
			case SSD:
				fread(&addr, sizeof(unsigned long), 1, s_stream);
				file_pointer += sizeof(unsigned long);
				fseek(s_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = SSD;
				
				//write signal into total checkpoint
				fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
				fflush(total_ckpt_stream);
				
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
						plist->addr = addr;
					}
					break;
				}
				else{
					fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
					fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
					fflush(total_ckpt_stream);
				}							
				break;
				
			case MD:
				fread(&addr, sizeof(unsigned long), 1, s_stream);
				file_pointer += sizeof(unsigned long);
				fseek(s_stream, file_pointer, SEEK_SET);
				current_ckpt_struct = MD;
				//write signal into total checkpoint
				fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
				fwrite(&current_ckpt_struct, sizeof(unsigned long), 1, total_ckpt_stream);
				fflush(total_ckpt_stream);				
				break;
		}
  		
  		//read data and write to total checkpoint
  		switch(current_ckpt_struct){
			case EP:
				len = PAGE_SIZE;
				//read data from checkpoint
				buff = (unsigned char *) malloc(len);
				fread(buff, len, 1,s_stream);
				file_pointer +=len;
				fseek(s_stream, file_pointer, SEEK_SET); 
				
				//write into total checkpoint
				fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
				fwrite(&addr, sizeof(unsigned long), 1, total_ckpt_stream); 		
				fwrite(buff, len, 1, total_ckpt_stream);
				fflush(total_ckpt_stream); 				
				break;
			case SSD:
				  //read len from checkpoint
				len = 0;
				fread(&len, sizeof(int), 1, s_stream);
				file_pointer += sizeof(int);
				fseek(s_stream, file_pointer, SEEK_SET);
				
				 //read data from checkpoint
				buff = (unsigned char *) malloc(len);
				fread(buff, len, 1,s_stream);
				file_pointer +=len;
				fseek(s_stream, file_pointer, SEEK_SET); 
  		
				//write into total checkpoint
				fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
				fwrite(&addr, sizeof(unsigned long), 1, total_ckpt_stream);
				//fflush(total_ckpt_stream);
				fwrite(&len, sizeof(int), 1, total_ckpt_stream);
				//fflush(total_ckpt_stream);
				fwrite(buff, len, 1, total_ckpt_stream);
				fflush(total_ckpt_stream); 				
				break;
			case SD:
				len = CAPE_WORD;
				//read data from checkpoint
				buff = (unsigned char *) malloc(len);
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
					fseek(total_ckpt_stream, total_ckpt_size,SEEK_SET);
					fwrite(&addr, sizeof(unsigned long), 1, total_ckpt_stream); 		
					fwrite(buff, len, 1, total_ckpt_stream);
					fflush(total_ckpt_stream); 
				}
												
				break;
			case MD:
				break;				
		}   			
 	}
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
	FILE *tmp_stream;
	unsigned char *tmp_ckpt;
	size_t tmp_size;


 	unsigned long file_pointer =  0, tmp_pointer=0;
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
 	
// 	if (final_list_ckpt_head != NULL)
//			clear_list_data_ckpt(final_list_ckpt_head); 	
//	final_list_ckpt_head =NULL;
//	final_list_ckpt_tail = NULL;
		
 	
 	//Copy total_ckpt to tmp_ckpt and close total_ckpt
	tmp_stream = open_binary_memstream(&tmp_ckpt, &tmp_size);
	fwrite(total_ckpt, total_ckpt_size, 1, tmp_stream);
	fflush(tmp_stream);
 	
	fclose(total_ckpt_stream);
	total_ckpt_size = 0;
 	 	
 	file_pointer = 0;
 	tmp_pointer =0 ;
 	
 	//1. Read timespan 	
 	fseek(tmp_stream, tmp_pointer, SEEK_SET);
 	fread(&t1, sizeof(unsigned long), 1, tmp_stream);
 	
 	fseek(src_ckpt_stream, file_pointer, SEEK_SET);
 	fread(&t2, sizeof(unsigned long), 1, src_ckpt_stream);
 	
 	total_ckpt_stream = open_binary_memstream(&total_ckpt, &total_ckpt_size); 
 	
 	printf("\nMERGE: Node %ld: total_ckpt = %zu; tmp_ckpt = %zu ; final_ckpt = %zu",node, total_ckpt_size, tmp_size, src_ckpt_size);
 	 	
 	if (t1 >= t2)
 	{
		//Write t1 and R1 into Total_ckpt
		fwrite(tmp_ckpt, sizeof(unsigned long) + sizeof(struct user_regs_struct), 1, total_ckpt_stream);
		fflush(total_ckpt_stream);
		
		file_pointer += sizeof(unsigned long) ;
		file_pointer += sizeof(struct user_regs_struct);
		//Write S2 into Total_ckpt
		merge_data(src_ckpt_stream, src_ckpt_data, src_ckpt_size, file_pointer, FINAL_CHECKPOINT);
		
		//Write S1 into Total_ckpt
		merge_data(tmp_stream, tmp_ckpt, tmp_size, file_pointer, TOTAL_CHECKPOINT);
		
	}
 	else
 	{
		//Write t2 and R2 into Total_ckpt
		fwrite(src_ckpt_data, sizeof(unsigned long) + sizeof(struct user_regs_struct), 1, total_ckpt_stream);
		fflush(total_ckpt_stream);
		
		file_pointer += sizeof(unsigned long) ;
		file_pointer += sizeof(struct user_regs_struct);
		
		//Write S1 into Total_ckpt
		merge_data(tmp_stream, tmp_ckpt, tmp_size, file_pointer, TOTAL_CHECKPOINT);
		
		//Write S2 into Total_ckpt
		merge_data(src_ckpt_stream, src_ckpt_data, src_ckpt_size, file_pointer, FINAL_CHECKPOINT);
	}
 	
 	
 	fclose(tmp_stream);
 	tmp_size = 0;
 	
//	join_checkpoint(TOTAL_CHECKPOINT, final_list_ckpt_head);
//	print_data_in_ckpt_list(final_list_ckpt_head);
 	
//	clear_list_data_ckpt(final_list_ckpt_head);
//	final_list_ckpt_head = NULL;
//	final_list_ckpt_tail = NULL;
 	
 		 	 	
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
		/* Master sends size and data to all slaves */
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

	return rc;
}
 
/*----------------------------------------------------------------------
 * require_scatter_checkpoint(): Version 4.0
 * 		Scatter workshare checkpoint to all node in system
 * ---------------------------------------------------------------------
 */
int require_scatter_checkpoint(){
	int rc = 0;
	int i;
	int chunk_size;

	if (node == 0 ){
		if (mbefore_ckpt_size <=0) return 1;
		task_ckpt_size = mbefore_ckpt_size / number_of_jobs ;
	}

	/* Broadcast task_ckpt_size from master to all slaves */
	if (node == 0) {
		for (i = 1; i < num_nodes; i++)
			cape_ucx_send(&task_ckpt_size, sizeof(int), i, TAG_SCATTER_SIZE);
	} else {
		cape_ucx_recv(&task_ckpt_size, sizeof(int), 0, TAG_SCATTER_SIZE);
	}

	chunk_size = jobs_per_node * task_ckpt_size;
	before_buffer = malloc(chunk_size * sizeof(char));

	/* Scatter: master sends each slave its chunk, keeps its own */
	if (node == 0) {
		memcpy(before_buffer, mbefore_ckpt, chunk_size);
		for (i = 1; i < num_nodes; i++)
			cape_ucx_send(mbefore_ckpt + i * chunk_size, chunk_size, i, TAG_SCATTER_DATA);
		fclose(mbefore_ckpt_stream);
		mbefore_ckpt_size = 0;
	} else {
		cape_ucx_recv(before_buffer, chunk_size, 0, TAG_SCATTER_DATA);
	}

	//initialize current job for all node
	current_job = 0;
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
			cape_ucx_send(&buffer_size, sizeof(int), i, TAG_BCAST_SIZE);
			cape_ucx_send(buffer_ckpt, buffer_size, i, TAG_BCAST_DATA);
		}
	}else{
		cape_ucx_recv(&buffer_size, sizeof(int), 0, TAG_BCAST_SIZE);
		buffer_ckpt = malloc(buffer_size);
		cape_ucx_recv(buffer_ckpt, buffer_size, 0, TAG_BCAST_DATA);
		//write buffer_ckpt to total_ckpt_stream file
		total_ckpt_stream = open_binary_memstream(&total_ckpt, &total_ckpt_size);
		fwrite(buffer_ckpt, buffer_size, 1, total_ckpt_stream);
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
		ckpt_size = 0;

		send_buffer = recv_buffer;
		message_size = recv_message_size;

	}
	free(recv_buffer);

	printf("\nMonitor %ld: after synchronized - total_ckpt_size = %zu\n", node, total_ckpt_size);

    return rc;
}


/*----------------------------------------------------------------------
 * hypercube_allreduce(): Allreduce using hypercube algorithm
 * 
 * ---------------------------------------------------------------------
 */
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
		printf("\nNode %ld - step %d- Total ckpt size = %zu - Send %d bytes to %d", node,i, total_ckpt_size,  send_msg_size, partner);


		//send size of message
		cape_ucx_sendrecv(&send_msg_size, sizeof(int), partner,
						  &recv_msg_size, sizeof(int), partner,
						  token_size);

		printf("\nNode %ld - step %d - Total ckpt size = %zu - Receive %d bytes from %d", node,i, total_ckpt_size,  recv_msg_size, partner);

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
		ckpt_size = 0;


		send_msg = total_ckpt;
		send_msg_size = total_ckpt_size;

	}


    return rc;
}

/*----------------------------------------------------------------------
 * require_allreduce_checkpoint(): version 4.0 => version 5
 * 		at each node, it collect checkpoints from other nodes
 * --------------------------------------------------------------------- 
 */ 
int require_allreduce_checkpoint(){
	int rc = 0;
	//rc = prepare_allreduce_checkpoint();	
	printf("\nPREPARE ALL REDUCE: Node %ld: TOTAL CHECKPOINT SIZE = %zu", node, total_ckpt_size);
	final_list_ckpt_head = list_ckpt_head;
	final_list_ckpt_tail = list_ckpt_tail;
//	print_data_in_ckpt_list(final_list_ckpt_head);
	
	
	rc=  merge_external_checkpoint(final_ckpt_stream, final_ckpt, final_ckpt_size);		
	
//	print_data_in_ckpt_list(list_ckpt_head);
	printf("\nBEFORE JOIN: Node %ld: TOTAL CHECKPOINT SIZE = %zu", node, total_ckpt_size);
	
	join_checkpoint(TOTAL_CHECKPOINT, final_list_ckpt_head);
	
	
	
	fclose(final_ckpt_stream);
	final_ckpt_size = 0;
	
	
	
	printf("\nAFTER JOIN: Node %ld: TOTAL CHECKPOINT SIZE = %zu", node, total_ckpt_size);
	
	
		
	if (is_power_of_two(num_nodes) && (total_ckpt_size < LARGE_CHECKPOINT)){
		//Use Hypercube algorithm
	    rc = hypercube_allreduce();
		//rc = ring_allreduce();
	}	
	else {	//Large file checkpoint or number of nodes is not power of 2	
		rc = ring_allreduce();
		 
	}	
	
//	printf("\nAFTER SYNCRONIZED: Node %ld: TOTAL CHECKPOINT SIZE = %d", node, total_ckpt_size);
//	join_checkpoint(TOTAL_CHECKPOINT, final_list_ckpt_head);
//	print_data_in_ckpt_list(final_list_ckpt_head);
//	printf("\nAFTER SYNCRONIZED and JOINED: Node %ld: TOTAL CHECKPOINT SIZE = %d", node, total_ckpt_size);

	return rc;
}
		
