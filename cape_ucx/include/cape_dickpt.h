#ifndef CAPE_DICKPT_H
#define CAPE_DICKPT_H
/*
 * cape_dickpt.h - CAPE dickpt application-side helpers (x86-64)
 *
 * Applications compiled for the dickpt monitor use these inline functions
 * to issue int3 breakpoints with a signal code in the rdx register.
 * The monitor (cape_incr.c) intercepts SIGTRAP via ptrace and reads rdx
 * to determine what action to take.
 *
 * Values returned by the monitor are read back from rdx.
 * Values sent to the monitor are placed in rax before the breakpoint.
 */

#include <stddef.h>

#include "cape_signal.h"

void dickpt_prepare_tracking(void);
void dickpt_register_region(void *addr, size_t len);
void *dickpt_map_region(size_t len);

/* ── Breakpoint helpers (x86-64) ───────────────────────────────────────── */
/* Issue a signal to the monitor: push rdx, set rdx=code, int3, pop rdx */
static inline void __cape_signal(int code)
{
    __asm__ volatile (
        "push %%rdx          \n\t"
        "movl %0, %%edx      \n\t"
        "int  $3             \n\t"
        "pop  %%rdx          \n\t"
        : /* no outputs */
        : "r"(code)
        : "memory"
    );
}

/* Send a long value to the monitor via rax, with signal code in rdx.
 * Use named-register variables so the compiler cannot alias `code` onto
 * %rax (which would be clobbered by the val load before edx is set). */
static inline void __cape_signal_val(int code, unsigned long val)
{
    register unsigned long _val asm("rax") = val;
    register unsigned long _code asm("rdx") = (unsigned long)(unsigned int)code;
    __asm__ volatile (
        "int $3"
        : "+r"(_val), "+r"(_code)
        :
        : "memory"
    );
}

/* Read a value back from the monitor via rdx. Use a named-register
 * variable bound to %rdx so input (code) and output (response) share
 * the right register without the compiler aliasing other inputs onto
 * %rdx and getting clobbered. */
static inline unsigned long __cape_signal_read(int code)
{
    register unsigned long _rdx asm("rdx") = (unsigned long)(unsigned int)code;
    __asm__ volatile (
        "int $3"
        : "+r"(_rdx)
        :
        : "memory"
    );
    return _rdx;
}

/* ── CAPE dickpt API ───────────────────────────────────────────────────── */

/* Memory locking / checkpointing */
static inline void dickpt_start_ckpt(void)
{
    dickpt_prepare_tracking();
    __cape_signal(S_LOCK_PROCESS_MEMORY);
}
static inline void dickpt_stop_ckpt(void)    { __cape_signal(S_UNLOCK_PROCESS_MEMORY); }
static inline void dickpt_generate_ckpt(void){ __cape_signal(S_GENERATE_CHECKPOINT); }
static inline void dickpt_generate_total_ckpt(void) { __cape_signal(S_GENERATE_TOTAL_CHECKPOINT); }
static inline void dickpt_generate_workshare_ckpt(void) { __cape_signal(S_GENERATE_WORKSHARE_CHECKPOINT); }

/* Send / Receive / Merge */
static inline void dickpt_send_ckpt(void)    { __cape_signal(S_SEND_CHECKPOINT); }
static inline void dickpt_receive_ckpt(void) { __cape_signal(S_RECEIVE_CHECKPOINT); }
static inline void dickpt_dispatch_task_ckpt(void) { __cape_signal(S_DISPATCH_TASK_CHECKPOINT); }
static inline unsigned long dickpt_receive_task_ckpt(void) { return __cape_signal_read(S_RECEIVE_TASK_CHECKPOINT); }

/* Register one task depend() item (type = CAPE_DEP_IN/OUT/INOUT). Issued by
 * every rank before the task block; only the master acts on it, building the
 * task DAG so dependent tasks are dispatched after their predecessors. */
static inline void dickpt_task_depend(void *addr, int type)
{
    register unsigned long _addr asm("rax") = (unsigned long)addr;
    register unsigned long _code asm("rdx") =
        (unsigned long)S_TASK_DEPEND
        | ((unsigned long)(unsigned int)type << 32);
    __asm__ volatile ("int $3" : "+r"(_addr), "+r"(_code) : : "memory");
}

/* ── Dynamic (nested/recursive) tasks ──────────────────────────────────────
 * The task body is outlined into a function void fn(void*); its by-value
 * captures (firstprivate) are packed into args. All ranks run the same
 * non-PIE binary with ASLR off, so the function pointer is valid on every
 * rank. See cape_signal.h for the master/worker protocol. */
typedef void (*dickpt_task_fn)(void *);

struct dickpt_task_desc {
    dickpt_task_fn fn;
    unsigned long args_size;
    unsigned char args[DICKPT_TASK_ARGS_MAX];
};

/* rax = &desc, rdx = code; monitor's reply comes back in rdx. */
static inline unsigned long __dickpt_task_signal(int code,
                                                 struct dickpt_task_desc *d)
{
    register unsigned long _rax asm("rax") = (unsigned long)d;
    register unsigned long _rdx asm("rdx") = (unsigned long)(unsigned int)code;
    __asm__ volatile ("int $3" : "+r"(_rax), "+r"(_rdx) : : "memory");
    return _rdx;
}

/* Spawn a task. Master: enqueued with the master's current delta checkpoint
 * folded into the accumulator. Worker: submitted to the master together with
 * this worker's delta checkpoint. Never blocks on worker availability. */
static inline void dickpt_task_spawn(dickpt_task_fn fn,
                                     const void *args, unsigned long n)
{
    struct dickpt_task_desc d;
    unsigned long i;
    d.fn = fn;
    d.args_size = n > DICKPT_TASK_ARGS_MAX ? DICKPT_TASK_ARGS_MAX : n;
    for (i = 0; i < d.args_size; i++)
        d.args[i] = ((const unsigned char *)args)[i];
    __dickpt_task_signal(S_TASK_SPAWN, &d);
}

/* Mark the current task finished: delta checkpoint goes to the master. */
static inline void dickpt_task_complete(void)
{
    dickpt_generate_ckpt();
    __cape_signal(S_TASK_COMPLETE);
}

/* Worker idle loop: blocks for the next task. Returns 1 with *d filled (the
 * task's input checkpoint is already injected) or 0 on region shutdown. */
static inline int dickpt_task_serve(struct dickpt_task_desc *d)
{
    return (int)__dickpt_task_signal(S_TASK_SERVE, d);
}

/* taskwait. On the master, blocks until its direct children completed.
 * On a worker, may be handed queued tasks to run inline while waiting;
 * returns once this task's children are done, their output injected. */
static inline void dickpt_task_wait(void)
{
    struct dickpt_task_desc d;
    if (__cape_signal_read(98) == 0) {   /* dickpt_read_node() */
        __dickpt_task_signal(S_TASK_WAIT, &d);
        return;
    }
    while (__dickpt_task_signal(S_TASK_WAIT, &d)) {
        d.fn(d.args);
        dickpt_task_complete();
    }
}

/* Master only: drain every outstanding task, then shut the workers' serve
 * loops down. Call at the end of the task region, before broadcast. */
static inline void dickpt_task_region_end(void)
{
    __cape_signal(S_TASK_REGION_END);
}

static inline void dickpt_inject_ckpt(void)  { __cape_signal(S_INJECT_CHECKPOINT); }
static inline void dickpt_inject_workshare_ckpt(void) { __cape_signal(S_INJECT_WORKSHARE_CHECKPOINT); }
static inline void dickpt_merge_ckpt(void)   { __cape_signal(S_MERGE_CHECKPOINT); }
static inline void dickpt_waitfor_ckpt(void) { __cape_signal(S_WAIT_FOR_CHECKPOINT); }

/* Collective */
static inline void dickpt_broadcast_ckpt(void) { __cape_signal(S_BROADCAST_CHECKPOINT); }
static inline void dickpt_scatter_ckpt(void)   { __cape_signal(S_SCATTER_CHECKPOINT); }
static inline void dickpt_allreduce_ckpt(void) { __cape_signal(S_ALL_REDUCE); }

/* Shared data */
static inline void dickpt_start_share_data(void) { __cape_signal(S_START_SHARE_DATA); }
static inline void dickpt_end_share_data(void)   { __cape_signal(S_END_SHARE_DATA); }

/* Register a shared region with the monitor's whitelist.
 *   addr/len  - byte range that is OpenMP-"shared" (or a reduction range)
 *   level     - lexical OMP scope depth (0=global/file, 1=parallel, 2=for/task, ...).
 *               dickpt_unregister_level() drops every entry with this exact level,
 *               matching the way OpenMP scopes nest and unwind.
 * Words outside any registered range are masked out of checkpoints, so a
 * private variable that happens to share a 4 KiB page with a shared one
 * is never shipped to an idle worker. */
static inline void dickpt_register_shared(void *addr, size_t len, unsigned char level)
{
    register unsigned long _addr  asm("rax") = (unsigned long)addr;
    register unsigned long _len   asm("rsi") = (unsigned long)len;
    register unsigned long _level asm("rcx") = (unsigned long)level;
    register unsigned long _code  asm("rdx") = (unsigned long)S_START_SHARE_DATA;
    __asm__ volatile ("int $3"
                      : "+r"(_addr), "+r"(_len), "+r"(_level), "+r"(_code)
                      :
                      : "memory");
}

static inline void dickpt_unregister_level(unsigned char level)
{
    register unsigned long _level asm("rax") = (unsigned long)level;
    register unsigned long _code  asm("rdx") = (unsigned long)S_END_SHARE_DATA;
    __asm__ volatile ("int $3"
                      : "+r"(_level), "+r"(_code)
                      :
                      : "memory");
}

/* Query monitor for rank/size info */
static inline unsigned long dickpt_read_node(void)      { return __cape_signal_read(98); }
static inline unsigned long dickpt_read_num_nodes(void)  { return __cape_signal_read(97); }
static inline unsigned long dickpt_read_ckpt_flag(void)  { return __cape_signal_read(96); }

/* Declare a scalar reduction variable. The monitor stores
 * (addr, datatype, op) in its hashmap; merge_bitmap_sections looks up
 * dirty word addresses and applies the op when both sides changed it. */
static inline void dickpt_declare_reduction(void *addr, int datatype, int op)
{
    register unsigned long _val asm("rax") = (unsigned long)addr;
    register unsigned long _code asm("rdx") =
        (unsigned long)S_DECLARE_REDUCTION
        | ((unsigned long)(unsigned char)datatype << 32)
        | ((unsigned long)(unsigned char)op       << 40);
    __asm__ volatile ("int $3" : "+r"(_val), "+r"(_code) : : "memory");
}

static inline void dickpt_declare_reduction_region(void *addr, size_t len,
                                                   int datatype, int op)
{
    register unsigned long _addr asm("rax") = (unsigned long)addr;
    register unsigned long _len asm("rsi") = (unsigned long)len;
    register unsigned long _code asm("rdx") =
        (unsigned long)S_DECLARE_REDUCTION_REGION
        | ((unsigned long)(unsigned char)datatype << 32)
        | ((unsigned long)(unsigned char)op       << 40);
    __asm__ volatile ("int $3"
                      : "+r"(_addr), "+r"(_len), "+r"(_code)
                      :
                      : "memory");
}

/* Send values to monitor */
static inline void dickpt_send_data_start(unsigned long addr) { __cape_signal_val(95, addr); }
static inline void dickpt_send_num_jobs(unsigned long n)      { __cape_signal_val(S_APP_SEND_NUMBER_OF_JOBS, n); }
static inline void dickpt_send_timespan(unsigned long t)      { __cape_signal_val(S_APP_SEND_TIMESPAN, t); }

#endif /* CAPE_DICKPT_H */
