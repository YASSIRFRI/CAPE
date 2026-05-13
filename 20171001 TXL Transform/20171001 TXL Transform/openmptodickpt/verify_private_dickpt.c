#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>
#define N 1024
static int data [N] __attribute__ ((aligned (4096)));

int main (int argc, char * * argv) {
    static int data [N] __attribute__ ((aligned (4096)));
    int rank = dickpt_read_node ();
    int nranks = dickpt_read_num_nodes ();
    int errors = 0;
    int i;
    int local;
    local = rank;
    {
        unsigned long __dickpt_node__ = dickpt_read_node ();
        unsigned long __dickpt_num_nodes__ = dickpt_read_num_nodes ();
        long long __dickpt_first__ = 0;
        long long __dickpt_last__ = N;
        long long __dickpt_count__ = __dickpt_last__ - __dickpt_first__;
        long long __dickpt_left__ = __dickpt_first__ + (__dickpt_count__ * __dickpt_node__) / __dickpt_num_nodes__;
        long long __dickpt_right__ = __dickpt_first__ + (__dickpt_count__ * (__dickpt_node__ + 1)) / __dickpt_num_nodes__;
        dickpt_send_num_jobs (__dickpt_count__);
        dickpt_register_region (& data, sizeof (data));
        dickpt_register_shared (& data, sizeof (data), 1);
        dickpt_start_ckpt ();
        for (i = __dickpt_left__; i < __dickpt_right__; i ++) {
            local = rank;
            data [i] = i * 2;
        }
        dickpt_generate_ckpt ();
        dickpt_allreduce_ckpt ();
        dickpt_stop_ckpt ();
        dickpt_unregister_level (1);
    }
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
}

