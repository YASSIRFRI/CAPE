/*
 * verify_private.c - end-to-end test that private memory is NOT shipped
 * between CAPE workers when a tracked page also contains shared memory.
 *
 * Setup: one global struct `pl` holds two arrays back-to-back:
 *   - pl.shared_results[]    : SHARED, written by each iteration's owner
 *   - pl.private_sentinels[] : PRIVATE, each rank stamps its own rank-tag
 *
 * Both arrays live in the same tracked region (so userfaultfd will fault
 * on writes to either) AND share 4 KiB pages. Only shared_results[] is
 * added to the monitor's whitelist via dickpt_register_shared.
 *
 * Before the parallel region every rank stamps a distinctive
 * (rank+1)*1000+i value into private_sentinels[]. The parallel loop
 * writes the canonical i*2 into shared_results[i] AND restamps the
 * private slot. After the loop completes and checkpoints merge:
 *
 *   - shared_results[i] must equal i*2 for every i (cross-rank agreement)
 *   - private_sentinels[i] must equal (myrank+1)*1000+i on each rank
 *     (i.e. NOT clobbered by another rank's value)
 *
 * Without the whitelist mask, rank R's dirty pages get shipped wholesale;
 * rank S then injects R's private_sentinels values over its own, and the
 * second assertion fails. With the mask, only words inside the shared
 * range are placed in the bitmap and shipped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

#include "../../../cape_ucx/include/cape_dickpt.h"

#define N 1024

struct payload {
    int shared_results[N];
    int private_sentinels[N];
};

static struct payload pl;

int main(int argc, char **argv) {
    unsigned long node, nnodes;
    int i;
    int errors = 0;

    /* Tracking: monitor watches every page in `pl` for writes.
     * Whitelist: only shared_results is allowed into checkpoints. */
    dickpt_register_region(&pl, sizeof(pl));
    dickpt_register_shared(&pl.shared_results[0],
                           sizeof(pl.shared_results),
                           0);

    node   = dickpt_read_node();
    nnodes = dickpt_read_num_nodes();

    /* Pre-stamp BOTH arrays before any checkpointing begins. The private
     * tag is rank-distinctive; if it survives the parallel region we know
     * no other rank's bytes were injected on top. */
    for (i = 0; i < N; i++) {
        pl.shared_results[i]    = -1;
        pl.private_sentinels[i] = (int)((node + 1) * 1000 + i);
    }

    #pragma omp parallel for
    for (i = 0; i < N; i++) {
        pl.shared_results[i] = i * 2;
        /* Re-touch the private slot in the same iteration so the
         * containing page IS dirty under userfaultfd. The mask, not
         * "page never faulted", is what must keep this private. */
        pl.private_sentinels[i] = (int)((node + 1) * 1000 + i);
    }

    for (i = 0; i < N; i++) {
        if (pl.shared_results[i] != i * 2) {
            if (errors < 10)
                fprintf(stderr,
                    "[rank %lu] shared_results[%d]=%d want %d\n",
                    node, i, pl.shared_results[i], i * 2);
            errors++;
        }
        int want_priv = (int)((node + 1) * 1000 + i);
        if (pl.private_sentinels[i] != want_priv) {
            if (errors < 10)
                fprintf(stderr,
                    "[rank %lu] private_sentinels[%d]=%d want %d "
                    "(PRIVATE CLOBBERED BY ANOTHER RANK)\n",
                    node, i, pl.private_sentinels[i], want_priv);
            errors++;
        }
    }

    if (errors == 0) {
        printf("RESULT rank=%lu/%lu status=OK\n", node, nnodes);
        return 0;
    }
    printf("RESULT rank=%lu/%lu status=FAIL errors=%d\n", node, nnodes, errors);
    return 1;
}
