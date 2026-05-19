#include <stdio.h>
#include <omp.h>

#define N 1024

static int data[N] __attribute__((aligned(4096)));

int main(int argc, char **argv) {
    int rank   = omp_get_thread_num();
    int nranks = omp_get_num_threads();
    int errors = 0;
    int i;
    int local;

    /* Pre-touch data so its page is physically resident before
     * dickpt_start_ckpt write-protects it; userfaultfd-wp only fires on
     * present pages, so BSS that's never been touched bypasses tracking. */
    for (i = 0; i < N; i++)
        data[i] = 0;

    local = rank;

    #pragma omp parallel for shared(data) private(local)
    for (i = 0; i < N; i++) {
        local = rank;
        data[i] = i * 2;
    }

    if (local != rank) {
        fprintf(stderr,
            "[rank %d] private local=%d want %d (CLOBBERED BY ANOTHER RANK)\n",
            rank, local, rank);
        errors++;
    }

    for (i = 0; i < N; i++) {
        if (data[i] != i * 2) {
            if (errors < 10)
                fprintf(stderr,
                    "[rank %d] data[%d]=%d want %d\n",
                    rank, i, data[i], i * 2);
            errors++;
        }
    }

    if (errors == 0) {
        printf("RESULT rank=%d/%d status=OK\n", rank, nranks);
        return 0;
    }
    printf("RESULT rank=%d/%d status=FAIL errors=%d\n", rank, nranks, errors);
    return 1;
}
