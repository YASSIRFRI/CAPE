#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../../include/cape_dickpt_uffd.h"
#include "../../include/cape_signal.h"

/*
 * DICKPT requires identical virtual addresses for tracked regions on every
 * rank. ASLR is disabled by the monitor via personality(ADDR_NO_RANDOMIZE)
 * before fork+exec of this app; the flag is inherited across execve, so the
 * app wakes up in a non-randomized address space with no dance required
 * here. Combined with -no-pie -fno-pie in the makefile, text/data/bss sit
 * at linker-assigned fixed VAs that match on all ranks.
 */

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

static struct cape_dickpt_range cape_ranges[CAPE_DICKPT_MAX_RANGES];
static size_t cape_range_count;
static int cape_tracking_ready;
/* Kept open after prepare_tracking so regions declared at runtime (e.g. a
 * task's shared() captures, which register after the enclosing region's
 * start_ckpt) can be UFFDIO_REGISTER'd on the same uffd the monitor holds. */
static int cape_uffd = -1;
static int cape_monitor_fd = -1;

static size_t cape_page_size(void)
{
	static size_t page_size;

	if (page_size == 0)
		page_size = (size_t)sysconf(_SC_PAGESIZE);
	return page_size;
}

static uint64_t cape_align_down(uint64_t value, uint64_t align)
{
	return value & ~(align - 1U);
}

static uint64_t cape_align_up(uint64_t value, uint64_t align)
{
	return (value + align - 1U) & ~(align - 1U);
}

static void cape_die_errno(const char *what)
{
	fprintf(stderr, "dickpt runtime: %s failed: %s\n", what, strerror(errno));
	exit(1);
}

static void cape_die_msg(const char *what)
{
	fprintf(stderr, "dickpt runtime: %s\n", what);
	exit(1);
}

static int cape_get_monitor_fd(void)
{
	const char *fd_text;
	char *endptr;
	long fd;

	fd_text = getenv(CAPE_DICKPT_ENV_SOCK_FD);
	if (fd_text == NULL || *fd_text == '\0')
		cape_die_msg("monitor control fd is not set");

	errno = 0;
	fd = strtol(fd_text, &endptr, 10);
	if (errno != 0 || endptr == fd_text || *endptr != '\0' || fd < 0)
		cape_die_msg("monitor control fd is invalid");

	return (int)fd;
}

static void cape_add_range(uint64_t start, uint64_t len)
{
	size_t i;
	uint64_t end;
	size_t page_size = cape_page_size();

	if (len == 0)
		return;

	start = cape_align_down(start, page_size);
	end = cape_align_up(start + len, page_size);
	len = end - start;

	for (i = 0; i < cape_range_count; ++i) {
		uint64_t current_start = cape_ranges[i].start;
		uint64_t current_end = current_start + cape_ranges[i].len;
		uint64_t merged_start;
		uint64_t merged_end;

		if (end < current_start || start > current_end)
			continue;

		merged_start = start < current_start ? start : current_start;
		merged_end = end > current_end ? end : current_end;
		cape_ranges[i].start = merged_start;
		cape_ranges[i].len = merged_end - merged_start;
		return;
	}

	if (cape_range_count >= CAPE_DICKPT_MAX_RANGES)
		cape_die_msg("too many checkpoint ranges");

	cape_ranges[cape_range_count].start = start;
	cape_ranges[cape_range_count].len = len;
	cape_range_count++;
}

/* True if [start, end) is already fully inside one tracked range. Used to
 * avoid re-issuing UFFDIO_REGISTER for a region that the enclosing scope
 * already registered (e.g. a task's shared() captures, which the parallel
 * region tracked first) — the kernel rejects a duplicate WP registration of
 * an already-registered range with EINVAL. */
static int cape_range_already_covered(uint64_t start, uint64_t end)
{
	size_t i;

	for (i = 0; i < cape_range_count; ++i) {
		uint64_t cs = cape_ranges[i].start;
		uint64_t ce = cs + cape_ranges[i].len;

		if (start >= cs && end <= ce)
			return 1;
	}
	return 0;
}

static void cape_register_stack_range(void)
{
	FILE *maps;
	char line[512];
	uintptr_t sp;
	size_t page_size = cape_page_size();

	sp = (uintptr_t)&maps;
	maps = fopen("/proc/self/maps", "r");
	if (maps == NULL)
		cape_die_errno("fopen(/proc/self/maps)");

	while (fgets(line, sizeof(line), maps) != NULL) {
		unsigned long long start, end;
		char perms[5];

		if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3)
			continue;
		if (!(perms[0] == 'r' && perms[1] == 'w'))
			continue;
		if (sp < start || sp >= end)
			continue;

		cape_add_range(start, cape_align_up(end - start, page_size));
		fclose(maps);
		return;
	}

	fclose(maps);
	cape_die_msg("failed to locate current stack mapping");
}

static int cape_create_userfaultfd(void)
{
	struct uffdio_api api;
	int uffd;

	uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	if (uffd < 0)
		uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0)
		cape_die_errno("userfaultfd");

	memset(&api, 0, sizeof(api));
	api.api = UFFD_API;
	api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
	if (ioctl(uffd, UFFDIO_API, &api) == -1)
		cape_die_errno("ioctl(UFFDIO_API)");
	if ((api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP) == 0)
		cape_die_msg("kernel does not support userfaultfd write-protect faults");

	return uffd;
}

static void cape_register_userfault_ranges(int uffd)
{
	size_t i;

	for (i = 0; i < cape_range_count; ++i) {
		struct uffdio_register reg;
		memset(&reg, 0, sizeof(reg));
		reg.range.start = cape_ranges[i].start;
		reg.range.len = cape_ranges[i].len;
		reg.mode = UFFDIO_REGISTER_MODE_WP;
		if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1)
			cape_die_errno("ioctl(UFFDIO_REGISTER)");
	}
}

static void cape_send_setup_to_monitor(int monitor_fd, int uffd)
{
	struct cape_dickpt_ctl_header header;
	struct cape_dickpt_range payload[CAPE_DICKPT_MAX_RANGES];
	struct iovec iov[2];
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} control;
	struct msghdr msg;
	struct cmsghdr *cmsg;

	memset(&header, 0, sizeof(header));
	header.type = CAPE_DICKPT_CTL_UFFD_SETUP;
	header.count = (uint32_t)cape_range_count;

	memcpy(payload, cape_ranges, cape_range_count * sizeof(payload[0]));

	memset(&msg, 0, sizeof(msg));
	memset(&control, 0, sizeof(control));

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);
	iov[1].iov_base = payload;
	iov[1].iov_len = cape_range_count * sizeof(payload[0]);
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = control.buf;
	msg.msg_controllen = sizeof(control.buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &uffd, sizeof(int));

	if (sendmsg(monitor_fd, &msg, 0) == -1)
		cape_die_errno("sendmsg(userfaultfd setup)");
}

/* Issue the S_REGISTER_REGION int3 signal: rax = aligned start, rsi = aligned
 * len, rdx = code. The monitor reads these registers on SIGTRAP, appends the
 * range to tracked_ranges, and write-protects it if tracking is live. */
static void cape_signal_register_region(uint64_t start, uint64_t len)
{
	register unsigned long _addr asm("rax") = (unsigned long)start;
	register unsigned long _len  asm("rsi") = (unsigned long)len;
	register unsigned long _code asm("rdx") = (unsigned long)S_REGISTER_REGION;
	__asm__ volatile ("int $3"
			  : "+r"(_addr), "+r"(_len), "+r"(_code)
			  :
			  : "memory");
}

/* Add the *exact* (un-page-aligned) byte range to the monitor's shared
 * whitelist (S_START_SHARE_DATA: rax=addr, rsi=len, rcx=level). Tracking
 * registration is page-aligned so writes fault, but the whitelist that
 * generate_checkpoint() consults to decide which words to ship must be exact:
 * a registered region is, by definition, shippable data, while a private
 * variable that merely shares a 4 KiB page with it is never registered and so
 * stays masked out. Without this, an app that only calls dickpt_register_region
 * (no explicit shared/reduction clause) would ship an empty checkpoint. */
static void cape_signal_register_shared(uint64_t addr, uint64_t len,
					unsigned long level)
{
	register unsigned long _addr  asm("rax") = (unsigned long)addr;
	register unsigned long _len   asm("rsi") = (unsigned long)len;
	register unsigned long _level asm("rcx") = level;
	register unsigned long _code  asm("rdx") = (unsigned long)S_START_SHARE_DATA;
	__asm__ volatile ("int $3"
			  : "+r"(_addr), "+r"(_len), "+r"(_level), "+r"(_code)
			  :
			  : "memory");
}

void dickpt_register_region(void *addr, size_t len)
{
	size_t page_size = cape_page_size();
	uint64_t start = cape_align_down((uint64_t)(uintptr_t)addr, page_size);
	uint64_t end   = cape_align_up((uint64_t)(uintptr_t)addr + (uint64_t)len,
				       page_size);
	uint64_t alen  = end - start;

	/* Whitelist the exact bytes so the region's contents are actually
	 * shipped. Level 1 (region scope); explicit shared()/reduction clauses
	 * add their own finer-grained entries and unwind independently. This is
	 * unconditional: tracking and the ship-whitelist are independent, and a
	 * region already tracked by an enclosing scope still needs its words in
	 * this scope's whitelist. */
	cape_signal_register_shared((uint64_t)(uintptr_t)addr, (uint64_t)len, 1);

	/* Already tracked by an enclosing scope (the common task-body case):
	 * the page range is registered on the uffd and known to the monitor, so
	 * re-registering would only earn an EINVAL from the kernel. Nothing more
	 * to do — the whitelist entry above is what this scope needed. */
	if (cape_range_already_covered(start, end))
		return;

	cape_add_range(start, alen);

	/* If tracking has already started (this is a genuinely new region
	 * declared inside an already-running checkpoint scope), the one-shot
	 * prepare_tracking() has already run, so register on the live uffd and
	 * tell the monitor now. Otherwise prepare_tracking() will pick it up. */
	if (cape_tracking_ready && cape_uffd >= 0) {
		struct uffdio_register reg;
		memset(&reg, 0, sizeof(reg));
		reg.range.start = start;
		reg.range.len = alen;
		reg.mode = UFFDIO_REGISTER_MODE_WP;
		if (ioctl(cape_uffd, UFFDIO_REGISTER, &reg) == -1 && errno != EEXIST)
			cape_die_errno("ioctl(UFFDIO_REGISTER) [runtime add]");
		cape_signal_register_region(start, alen);
	}
}

/*
 * All DICKPT nodes must map their tracked region at the SAME virtual address
 * so that checkpoint diffs (which contain absolute addresses) can be injected
 * into any node's address space after allreduce.
 *
 * We use a fixed hint address in a range that is typically unused by the
 * default mmap region and the heap.  MAP_FIXED_NOREPLACE (Linux 4.17+)
 * fails cleanly if the range is already occupied.
 */
#ifndef DICKPT_FIXED_MAP_ADDR
#define DICKPT_FIXED_MAP_ADDR  ((void *)0x200000000000UL)  /* 32 TiB */
#endif

void *dickpt_map_region(size_t len)
{
	void *addr;
	size_t page_size = cape_page_size();
	size_t aligned_len = (size_t)cape_align_up((uint64_t)len, page_size);
	static char *next_fixed = NULL;

	if (next_fixed == NULL)
		next_fixed = (char *)DICKPT_FIXED_MAP_ADDR;

	addr = mmap(next_fixed, aligned_len, PROT_READ | PROT_WRITE,
	            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (addr == MAP_FAILED) {
		/* Fall back: let kernel choose (inject may not work across nodes) */
		addr = mmap(NULL, aligned_len, PROT_READ | PROT_WRITE,
		            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (addr == MAP_FAILED)
			return NULL;
	}

	next_fixed = (char *)addr + aligned_len;
	dickpt_register_region(addr, aligned_len);
	return addr;
}

void dickpt_prepare_tracking(void)
{
	int monitor_fd;
	int uffd;

	if (cape_tracking_ready)
		return;

	/* Do NOT track the stack — it is node-local state at different
	 * addresses on each node.  Only dickpt_map_region / dickpt_register_region
	 * regions (shared data) should be checkpointed and exchanged. */
	/* cape_register_stack_range(); */
	monitor_fd = cape_get_monitor_fd();
	uffd = cape_create_userfaultfd();
	cape_register_userfault_ranges(uffd);
	cape_send_setup_to_monitor(monitor_fd, uffd);
	/* Keep uffd open: regions registered later (task scope) reuse it. */
	cape_uffd = uffd;
	cape_monitor_fd = monitor_fd;
	cape_tracking_ready = 1;
}
