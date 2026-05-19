#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape.h"
#include <stdio.h>
#define N 1024
static int data [N] __attribute__ ((aligned (4096)));

int main (int argc, char * * argv) {
    static int data [N] __attribute__ ((aligned (4096)));
    cape_declare_variable (& data, CAPE_INT, N, 0);
    cape_init ();
    int rank = cape_get_node_num ();
    int nranks = cape_get_num_nodes ();
    int errors = 0;
    int i;
    int local;
    for (i = 0; i < N; i ++) data [i] = 0;
    local = rank;
    cape_begin (PARALLEL_FOR, 0, N);
    cape_set_shared (& data);
    cape_set_private (& local);
    for (i = __left__; i < __right__; i ++) {
        local = rank;
        data [i] = i * 2;
    }
    cape_end (PARALLEL_FOR, FALSE);
    if (local != rank) {
        fprintf (stderr, "[rank %d] private local=%d want %d (CLOBBERED BY ANOTHER RANK)\n", rank, local, rank);
        errors ++;
    }
    for (i = 0; i < N; i ++) {
        if (data [i] != i * 2) {
            if (errors < 10) fprintf (stderr, "[rank %d] data[%d]=%d want %d\n", rank, i, data [i], i * 2);
            errors ++;
        }
    }
    if (errors == 0) {
        printf ("RESULT rank=%d/%d status=OK\n", rank, nranks);
        return 0;
    }
    printf ("RESULT rank=%d/%d status=FAIL errors=%d\n", rank, nranks, errors);
    return 1;
    cape_finalize ();
}

