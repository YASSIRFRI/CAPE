/*
 * verify_private.c - end-to-end correctness test for the CAPE bitmap monitor's
 * private-memory mask. Pure OpenMP source; the omptodickpt transpiler injects
 * dickpt_register_region/dickpt_register_shared for each variable in the
 * shared(...) clause.
 *
 * Two page-aligned globals:
 *   - shared_arr[]   : listed in shared(...). Transpiler registers it both
 *                      for userfaultfd tracking and for the shared-data
 *                      whitelist. Each iteration writes shared_arr[i] = i*2.
 *   - private_arr[]  : NOT listed in any shared(...) clause. Therefore it is
 *                      never registered with the monitor, never tracked, and
 *                      its contents must not appear in any rank's checkpoint.
 *                      Each rank stamps a rank-distinctive pattern into it
 *                      before AND inside the parallel region.
 *
 * Post-conditions verified on every rank:
 *   - shared_arr[i] == i*2 for every i        (cross-rank merge succeeded)
 *   - private_arr[i] == myrank*1000 + i       (no other rank's bytes leaked in)
 */

#include <stdio.h>
#include <omp.h>

#define NPER 1024

static int shared_arr[NPER]  __attribute__((aligned(4096)));
static int private_arr[NPER] __attribute__((aligned(4096)));

int main(int argc, char **argv) {
    int i;
    int my_rank   = omp_get_thread_num();
    int n_ranks   = omp_get_num_threads();
    int errors    = 0;

    for (i = 0; i < NPER; i++) {
        shared_arr[i]  = -1;
        private_arr[i] = my_rank * 1000 + i;
    }

    #pragma omp parallel for shared(shared_arr)
    for (i = 0; i < NPER; i++) {
        shared_arr[i] = i * 2;
    }

    for (i = 0; i < NPER; i++) {
        int want_priv = my_rank * 1000 + i;
        if (shared_arr[i] != i * 2) {
            if (errors < 10)
                fprintf(stderr,
                    "[rank %d] shared_arr[%d]=%d want %d\n",
                    my_rank, i, shared_arr[i], i * 2);
            errors++;
        }
        if (private_arr[i] != want_priv) {
            if (errors < 10)
                fprintf(stderr,
                    "[rank %d] private_arr[%d]=%d want %d "
                    "(PRIVATE CLOBBERED BY ANOTHER RANK)\n",
                    my_rank, i, private_arr[i], want_priv);
            errors++;
        }
    }

    if (errors == 0) {
        printf("RESULT rank=%d/%d status=OK\n", my_rank, n_ranks);
        return 0;
    }
    printf("RESULT rank=%d/%d status=FAIL errors=%d\n", my_rank, n_ranks, errors);
    return 1;
}
