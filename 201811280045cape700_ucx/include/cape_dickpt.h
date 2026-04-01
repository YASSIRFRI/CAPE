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

#include "cape_signal.h"

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

/* Send a long value to the monitor via rax, with signal code in rdx */
static inline void __cape_signal_val(int code, unsigned long val)
{
    __asm__ volatile (
        "push %%rax          \n\t"
        "push %%rdx          \n\t"
        "movq %0, %%rax      \n\t"
        "movl %1, %%edx      \n\t"
        "int  $3             \n\t"
        "pop  %%rdx          \n\t"
        "pop  %%rax          \n\t"
        : /* no outputs */
        : "r"(val), "r"(code)
        : "memory"
    );
}

/* Read a value back from the monitor via rdx */
static inline unsigned long __cape_signal_read(int code)
{
    unsigned long result;
    __asm__ volatile (
        "push %%rdx          \n\t"
        "movl %1, %%edx      \n\t"
        "int  $3             \n\t"
        "movq %%rdx, %0      \n\t"
        "pop  %%rdx          \n\t"
        : "=r"(result)
        : "r"(code)
        : "memory"
    );
    return result;
}

/* ── CAPE dickpt API ───────────────────────────────────────────────────── */

/* Memory locking / checkpointing */
static inline void dickpt_start_ckpt(void)   { __cape_signal(S_LOCK_PROCESS_MEMORY); }
static inline void dickpt_stop_ckpt(void)    { __cape_signal(S_UNLOCK_PROCESS_MEMORY); }
static inline void dickpt_generate_ckpt(void){ __cape_signal(S_GENERATE_CHECKPOINT); }
static inline void dickpt_generate_total_ckpt(void) { __cape_signal(S_GENERATE_TOTAL_CHECKPOINT); }
static inline void dickpt_generate_workshare_ckpt(void) { __cape_signal(S_GENERATE_WORKSHARE_CHECKPOINT); }

/* Send / Receive / Merge */
static inline void dickpt_send_ckpt(void)    { __cape_signal(S_SEND_CHECKPOINT); }
static inline void dickpt_receive_ckpt(void) { __cape_signal(S_RECEIVE_CHECKPOINT); }
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

/* Query monitor for rank/size info */
static inline unsigned long dickpt_read_node(void)      { return __cape_signal_read(98); }
static inline unsigned long dickpt_read_num_nodes(void)  { return __cape_signal_read(97); }
static inline unsigned long dickpt_read_ckpt_flag(void)  { return __cape_signal_read(96); }

/* Send values to monitor */
static inline void dickpt_send_data_start(unsigned long addr) { __cape_signal_val(95, addr); }
static inline void dickpt_send_num_jobs(unsigned long n)      { __cape_signal_val(S_APP_SEND_NUMBER_OF_JOBS, n); }
static inline void dickpt_send_timespan(unsigned long t)      { __cape_signal_val(S_APP_SEND_TIMESPAN, t); }

#endif /* CAPE_DICKPT_H */
