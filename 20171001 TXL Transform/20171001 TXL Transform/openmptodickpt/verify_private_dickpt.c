#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>
#define NPER 1024
static int shared_arr [NPER] __attribute__ ((aligned (4096)));
static int private_arr [NPER] __attribute__ ((aligned (4096)));

int main (int argc, char * * argv) {
    static int shared_arr [NPER] __attribute__ ((aligned (4096)));
    static int private_arr [NPER] __attribute__ ((aligned (4096)));
    int i;
    int my_rank = dickpt_read_node ();
    int n_ranks = dickpt_read_num_nodes ();
    int errors = 0;
    for (i = 0; i < NPER; i ++) {
        shared_arr [i] = -1;
        private_arr [i] = my_rank * 1000 + i;
    }
    {
        unsigned long __dickpt_node__ = dickpt_read_node ();
        unsigned long __dickpt_num_nodes__ = dickpt_read_num_nodes ();
        long long __dickpt_first__ = 0;
        long long __dickpt_last__ = NPER;
        long long __dickpt_count__ = __dickpt_last__ - __dickpt_first__;
        long long __dickpt_left__ = __dickpt_first__ + (__dickpt_count__ * __dickpt_node__) / __dickpt_num_nodes__;
        long long __dickpt_right__ = __dickpt_first__ + (__dickpt_count__ * (__dickpt_node__ + 1)) / __dickpt_num_nodes__;
        dickpt_send_num_jobs (__dickpt_count__);
        dickpt_register_region (& shared_arr, sizeof (shared_arr));
        dickpt_register_shared (& shared_arr, sizeof (shared_arr), 1);
        dickpt_start_ckpt ();
        for (i = __dickpt_left__; i < __dickpt_right__; i ++) {
            shared_arr [i] = i * 2;
        }
        dickpt_generate_ckpt ();
        dickpt_allreduce_ckpt ();
        dickpt_stop_ckpt ();
        dickpt_unregister_level (1);
    }
    for (i = 0; i < NPER; i ++) {
        int want_priv = my_rank * 1000 + i;
        if (shared_arr [i] != i * 2) {
            if (errors < 10) fprintf (stderr, "[rank %d] shared_arr[%d]=%d want %d\n", my_rank, i, shared_arr [i], i * 2);
            errors ++;
        }
        if (private_arr [i] != want_priv) {
            if (errors < 10) fprintf (stderr, "[rank %d] private_arr[%d]=%d want %d " "(PRIVATE CLOBBERED BY ANOTHER RANK)\n", my_rank, i, private_arr [i],
              want_priv);
            errors ++;
        }
    }
    if (errors == 0) {
        printf ("RESULT rank=%d/%d status=OK\n", my_rank, n_ranks);
        return 0;
    }
    printf ("RESULT rank=%d/%d status=FAIL errors=%d\n", my_rank, n_ranks, errors);
    return 1;
}

