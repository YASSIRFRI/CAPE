# Report Guideline: CAPE, DICKPT, and Bitmap Incremental Checkpointing

This document is a guideline for writing the end-of-studies report about this project. It should not be copied as a final report verbatim. Its purpose is to give the report-writing model the technical story, the design motivations, the implementation choices, and the important workarounds. The final report should remain explanatory and should not include code snippets.

## Project Goal

The project extends CAPE, Checkpointing Aided Parallel Execution, from an OpenMP-to-distributed-execution transformation into a runtime capable of executing OpenMP-like programs across several processes or nodes. The central idea is to replace shared-memory parallel execution with a distributed model where each rank executes a transformed version of the original program, produces checkpoints of the memory it modified, exchanges those checkpoints with other ranks, merges them, and injects the merged state back into the local process.

The work has two related runtime families:

- DICKPT, the discontinuous checkpointer used by cape_incr.c and cape_incr_bitmap.c. This is the two-executable model: an application process is supervised by an external monitor process.
- CAPE with timestamps, represented by cape_bitmap.c. This keeps the cape.h programming interface and links the runtime into the same executable as the transformed application.

Both paths implement incremental checkpointing. Instead of copying the whole address space, they detect pages written during a parallel section, compare the page before and after execution, serialize only the modified state, exchange it with UCX, merge remote updates, and inject the result.

## High-Level Translation Approach

The source-to-source transformation is implemented with TXL. The original OpenMP program is parsed with the OpenMP grammar and rewritten either into CAPE calls or DICKPT calls.

For CAPE, the transformer produces a program that calls cape_init at startup, cape_begin at the start of translated OpenMP constructs, cape_end at the end of those constructs, and cape_finalize at shutdown. Array declarations are preserved and augmented with cape_declare_variable so the runtime knows which memory regions should be tracked. Scalars are generally left alone because stack-local scalar variables are naturally private to each rank.

For DICKPT, the transformer produces a program that includes cape_dickpt.h and communicates with the monitor through breakpoint-based signals. The monitor handles lifecycle, communication, checkpoint generation, merging, and injection. OpenMP environment routines become DICKPT rank and size queries. Worksharing constructs compute rank-local ranges, so each rank executes only its slice. Outermost parallel constructs own the checkpoint lifecycle: start tracking, execute work, generate a checkpoint, allreduce checkpoints, then stop tracking. Inner constructs are mostly control-flow rewrites and do not restart checkpoint tracking.

The important conceptual difference is timestamping. CAPE uses explicit runtime checkpoint boundaries created by cape_begin and cape_end. DICKPT uses the breakpoint location itself as the synchronization identity: all ranks execute the same text, so reaching the same instrumented program counter means they reached the same logical point.

## DICKPT Without Bitmap: cape_incr.c

cape_incr.c is the baseline DICKPT monitor. It keeps the external-monitor architecture and implements discontinuous incremental checkpointing without the compact bitmap section format.

The application calls inline helpers from cape_dickpt.h. These helpers place a command identifier in a register and execute a breakpoint instruction. The monitor traces the child process with ptrace, catches the breakpoint, reads the command, and dispatches to the corresponding runtime action: lock memory, generate checkpoint, send, receive, inject, wait, broadcast, allreduce, or provide rank information.

At startup, the monitor forks the application. Before exec, it disables address space layout randomization so that globals, static data, and fixed mappings have matching virtual addresses on all ranks. This is necessary because checkpoint records store absolute virtual addresses, and injection only works if the same address means the same object on every rank. The application creates a userfaultfd instance, registers the memory ranges that must be tracked, and passes the userfaultfd file descriptor plus range metadata to the monitor through a Unix socket. This is a major workaround: userfaultfd belongs to the application, but the monitor must be able to receive and process its write-protection fault events.

When tracking starts, the monitor write-protects all registered ranges. On the first write to a protected page, the kernel sends a userfaultfd write-protect fault. The monitor snapshots the old 4 KiB page with process_vm_readv, stores it in a sorted dirty-page list, and then removes write protection for that page so the application can continue. At checkpoint time, the monitor drains pending userfaultfd events, reads each dirty page again, compares the saved pre-image with the current post-image, and serializes the changed memory.

The non-bitmap checkpoint format is record-oriented. It begins with a timestamp field and saved registers, then stores modified data using legacy markers for full pages, single words, and variable-length sub-page ranges. The generator scans a dirty page word by word, finds contiguous changed regions, writes their address, length when needed, and data. It then re-arms write protection for pages that were captured and clears the dirty-page list.

Merging in this version follows the older CAPE checkpoint structure. The merge compares the timestamp field of the current total checkpoint and the incoming checkpoint, keeps the newer header and register state, and combines the payload sections. It also has legacy support for shared-data and reduction lists. However, in the userfaultfd version of cape_incr.c, the shared-data metadata path is effectively not implemented: the handler for shared-data registration only logs that the backend does not consume that metadata. This is an important limitation to mention. The non-bitmap runtime is useful as a historical baseline and as a simpler incremental checkpoint prototype, but it is weaker for precise OpenMP data-sharing semantics.

The main workarounds in cape_incr.c are:

- userfaultfd is used instead of mprotect plus SIGSEGV because it provides explicit write-protect faults and lets the monitor capture dirty pages without permanently crashing or delivering normal segmentation faults to the child.
- process_vm_readv and process_vm_writev are used to read and patch the traced child without injecting helper code into the child process.
- ASLR is disabled and application binaries are expected to use stable virtual addresses, because checkpoint injection is address-based.
- open_memstream is used for writing checkpoint buffers, but fmemopen is required when reading those buffers back; this avoids the common bug where an open_memstream buffer is treated as a readable stream.
- memory buffers from open_memstream are explicitly freed after sending or merging, because repeated checkpoint phases otherwise accumulate large monitor memory usage.
- UCX is initialized in the monitor after fork, not before, to avoid slow or invalid memory registration behavior caused by copy-on-write after fork.

## DICKPT With Bitmap: cape_incr_bitmap.c

cape_incr_bitmap.c is the improved DICKPT monitor. It keeps the same external-monitor architecture, breakpoint protocol, userfaultfd dirty-page tracking, and UCX communication, but changes the checkpoint payload representation and the merge semantics.

The key motivation for the bitmap version is precision. A dirty page may contain both shared variables and private variables. It may also contain disjoint writes from different ranks inside the same 4 KiB page. A page-granular or coarse sub-page merge can overwrite useful work or ship private state to a rank that never executed the task. The bitmap format fixes this by tracking changed words inside each dirty page and by masking out words that are not declared shared.

The bitmap checkpoint section stores:

- a bitmap section marker;
- the number of dirty pages that actually contain exported changes;
- the sorted page addresses;
- for each page, a 128-byte bitmap with one bit per 4-byte word;
- the changed 4-byte words packed in bitmap scan order.

This gives 1024 bits per 4 KiB page, so the runtime can describe exactly which 4-byte words changed. Pages that faulted but have no exported changed words are omitted.

The generation path still begins with userfaultfd. The monitor stores the pre-image of a page on first write, then at checkpoint time reads the live post-image from the child process. For each 4-byte word, it first checks whether the address belongs to a registered shared region. If not, the word is ignored even if it changed. If yes and the word changed, the corresponding bitmap bit is set and the new word value is appended to the payload.

The shared-region whitelist is populated by the transformed DICKPT program. For a shared variable, the generated program registers the memory range both for userfaultfd tracking and as a shared range. The shared-range record includes a lexical level, so scope-local entries can be removed at the end of the OpenMP scope. The same list is also used for reductions. Reduction declarations record the address, data type, operation, and optional range length.

The bitmap merge is a two-pointer merge over sorted page-address arrays. A page present on only one side is emitted directly. A page present on both sides is unpacked into word arrays. For each word:

- if only one checkpoint changed it, that value is kept;
- if both checkpoints changed it and the word is a declared reduction variable, the reduction operation is applied;
- if both checkpoints changed it and it is not a reduction, the newer checkpoint wins according to the timestamp ordering.

This is the main semantic improvement. Disjoint writes inside the same page now merge correctly, which is important for data-parallel loops and for sparse writes to arrays. Reduction support is moved into the bitmap merge itself, which avoids maintaining a separate legacy L section for reduction words.

Injection is also bitmap-based. The monitor reads the target page from the child, patches only the words whose bits are set, and writes the full page back. Before injection, tracked ranges are temporarily made writable, then write protection is restored afterward if tracking is still active. This avoids userfaultfd faults caused by the monitor's own injection writes.

cape_incr_bitmap.c also improves task support. The master rank keeps a dynamic worker pool. A task dispatch sends either a run message with a checkpoint payload or a skip message to workers. A worker that receives a task injects the checkpoint, runs the task body, generates a new checkpoint, and sends it back. The master receives completions using UCX wildcard matching and identifies the worker from the sender tag. This avoids relying only on a fixed round-robin worker order.

The implementation includes several performance and robustness workarounds:

- UCX scratch buffers are allocated and memory-mapped once to reduce repeated registration overhead for large checkpoint messages.
- Single-message probed receives are used in the bitmap path, so the receiver discovers message length from UCX instead of sending a separate size message first.
- Hypercube allreduce is used when the number of ranks is a power of two; ring allreduce is used otherwise.
- The monitor can pin itself and the child to CPU sets using scheduler affinity, reducing interference between computation and monitoring.
- The child resets dumpability before ptrace setup, because some launchers make processes non-dumpable and the kernel can then reject PTRACE_TRACEME.
- File-system UCX bootstrap uses careful ready/go markers and directory synchronization to avoid stale NFS lookup behavior.
- The bitmap popcount implementation is adjusted in cape_bitmap.c to avoid libgcc and binutils incompatibilities on the target cluster.

The report should motivate the bitmap version as both a correctness improvement and a performance improvement. It reduces checkpoint size for sparse updates, avoids private-memory corruption, and supports word-level merging for multiple writers to different words in the same page.

## CAPE With Timestamps: cape_bitmap.c

cape_bitmap.c is the in-process CAPE runtime backend. It is compiled together with the transformed CAPE application and exposes the cape.h interface. Unlike DICKPT, there is no external monitor and no ptrace breakpoint protocol. The transformed application calls CAPE runtime functions directly.

The transformed CAPE program begins with cape_init and ends with cape_finalize. OpenMP constructs are translated into cape_begin and cape_end calls. For parallel loops, cape_begin computes the rank-local loop bounds and stores them in global helper variables consumed by the transformed loop. The transformed source then executes only the slice owned by the current rank.

Memory registration is different from DICKPT. CAPE declaration rewriting preserves arrays and appends cape_declare_variable calls. cape_declare_variable registers the array's memory range for userfaultfd tracking and marks it as shared. Scalars are usually not registered, because they are stack-local and should remain private unless explicitly hoisted into a task environment.

The in-process runtime still uses userfaultfd write-protection. During cape_start_ckpt, registered ranges are write-protected. A helper thread polls the userfaultfd and captures dirty page pre-images directly with memcpy because the runtime is in the same address space as the application. A mutex protects the dirty-page list because both the helper thread and checkpoint-generation path may drain events. At cape_end, the runtime drains remaining events, generates the checkpoint, stops write protection, performs the UCX allreduce, and injects the merged checkpoint.

The checkpoint format in cape_bitmap.c has been updated to the same word-level bitmap payload used in cape_incr_bitmap.c. It stores a timestamp-like header field and a placeholder register block, followed by a bitmap section. Registers are not meaningful in this in-process model, so the register area is kept mostly for format compatibility. The merge reads the timestamp fields of the two checkpoints, chooses the newer header, and performs the same word-level bitmap merge with reduction support.

The timestamp story should be explained carefully in the report. Conceptually, CAPE uses explicit logical timestamps because each cape_end marks a runtime synchronization point. The file contains a logical program counter and timestamp variables, and cape_end increments them. The checkpoint format also stores a time field used by merge_external_checkpoint for ordering. In the current bitmap backend, the stored field is the timespan variable rather than directly storing the logical timestamp variable. A report can present this as a legacy compatibility detail of the prototype and note that the intended model is explicit CAPE synchronization timestamps, whereas DICKPT uses the breakpoint program counter as the implicit synchronization identity.

Task support in CAPE is handled by a source-level closure transformation. A task with shared captures is rewritten to allocate a task environment structure at a stable fixed virtual address, copy captured values into it, execute the task body through that environment, and copy results back. cape_task_env_alloc maps the environment at a deterministic address, registers it for tracking, and marks it as shared. This avoids a distributed task accessing a parent stack frame whose address may not be meaningful or safe on another rank.

The main CAPE-specific workarounds are:

- userfaultfd is used inside the same process, so a helper thread must drain faults while the application continues.
- checkpoint generation uses memcpy instead of process_vm_readv because the runtime can access its own address space directly.
- arrays are registered and marked shared, while scalars remain private by default.
- task captures are hoisted into a stable heap-like mapped region to avoid stack-address problems.
- the bitmap merge is reused to solve sub-page disjoint writes and reductions.
- cape_finalize must stop the helper thread, close userfaultfd and epoll descriptors, free tracked ranges, finalize UCX, and report profiling data.

## Challenges to Emphasize in the Report

The first challenge is preserving OpenMP semantics while moving from threads to distributed ranks. OpenMP shared memory allows multiple threads to see the same address space. In this project, each rank is a separate process, so the runtime must reconstruct a shared-memory illusion by checkpointing and merging memory updates.

The second challenge is identifying the correct memory to exchange. Sending whole process memory is too expensive and unsafe. Sending whole dirty pages is also unsafe because a dirty page can contain private and shared data at the same time. The shared whitelist and word-level bitmap are therefore central to correctness.

The third challenge is stable virtual addressing. Checkpoints contain addresses, so ranks must agree on virtual addresses for shared objects and task environments. This motivates disabling ASLR, using non-PIE style assumptions, and mapping task or DICKPT regions at fixed addresses.

The fourth challenge is merging concurrent updates. Data-parallel loops usually write disjoint ranges, but those ranges can share pages. The bitmap merge can union disjoint word updates and apply reduction operators when the same reduction variable is updated by multiple ranks.

The fifth challenge is runtime overhead. Userfaultfd, ptrace, process_vm_readv, process_vm_writev, UCX communication, and repeated memory allocation can all dominate execution if implemented naively. The project therefore uses incremental tracking, compact bitmap payloads, UCX direct communication, scratch buffers, probed receives, and profiling counters.

The sixth challenge is OpenMP tasks. A task may capture variables from a parent stack frame. That does not translate cleanly to distributed ranks. The task-environment workaround converts captures into a stable shared structure that can be checkpointed and injected.

## Suggested Report Structure

A good final report can use this structure:

1. Introduce CAPE and DICKPT. Explain the goal of executing OpenMP-style programs on distributed ranks through checkpointing.
2. Explain the TXL transformation. Describe how OpenMP constructs, clauses, environment routines, loops, sections, tasks, and reductions are rewritten.
3. Present the DICKPT architecture without bitmap. Focus on the external monitor, breakpoint protocol, userfaultfd dirty tracking, non-bitmap checkpoint records, UCX exchange, and limitations.
4. Motivate and present the bitmap DICKPT implementation. Emphasize private-memory masking, word-level dirty maps, reduction-aware merging, task dispatch, and communication optimizations.
5. Present CAPE with timestamps in cape_bitmap.c. Contrast the in-process library model with the external DICKPT monitor and explain cape_begin/cape_end synchronization.
6. Discuss implementation challenges and workarounds: stable addresses, ASLR, userfaultfd setup, ptrace permissions, open_memstream/fmemopen, UCX bootstrap, scratch buffers, and task environments.
7. Evaluate expected benefits and limits. The bitmap approach should reduce checkpoint size and avoid false sharing, but it still depends on correct memory registration, fixed virtual addresses, and careful transform coverage.

## Key Takeaway

The project evolves CAPE from coarse checkpoint-based distributed OpenMP execution toward a more precise incremental model. DICKPT without bitmap demonstrates the basic external-monitor checkpointing pipeline. DICKPT with bitmap makes that pipeline practical for sparse and sub-page updates by exporting only shared dirty words and merging them with reduction awareness. CAPE with timestamps adapts the same bitmap ideas to the original in-process CAPE interface, where explicit cape_begin and cape_end calls define synchronization points.
