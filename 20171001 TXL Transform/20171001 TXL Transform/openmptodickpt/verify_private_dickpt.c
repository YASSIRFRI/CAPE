#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#define N 1024
struct payload {
    int shared_results [N];
    int private_sentinels [N];
};
static struct payload pl;

int main (int argc, char * * argv) {
    struct payload {
        int shared_results [N];
        int private_sentinels [N];
    };
    static struct payload pl;
    unsigned long node, nnodes;
    int i;
    int errors = 0;
    dickpt_register_region (& pl, sizeof (pl));
    dickpt_register_shared (&pl.shared_results[0], sizeof (pl.shared_results), 0);
    node = dickpt_read_node ();
    nnodes = dickpt_read_num_nodes ();
    for (i = 0; i < N; i ++) {
        pl.shared_results [i] = -1;
        pl.private_sentinels [i] = (int) ((node + 1) * 1000 + i);
    }
    {
        unsigned long __dickpt_node__ = dickpt_read_node ();
        unsigned long __dickpt_num_nodes__ = dickpt_read_num_nodes ();
        long long __dickpt_first__ = 0;
        long long __dickpt_last__ = N;
        long long __dickpt_count__ = __dickpt_last__ - __dickpt_first__;
        long long __dickpt_left__ = __dickpt_first__ + (__dickpt_count__ * __dickpt_node__) / __dickpt_num_nodes__;
        long long __dickpt_right__ = __dickpt_first__ + (__dickpt_count__ * (__dickpt_node__ + 1)) / __dickpt_num_nodes__;
        dickpt_send_num_jobs (__dickpt_count__);
        dickpt_start_ckpt ();
        for (i = __dickpt_left__; i < __dickpt_right__; i ++) {
            pl.shared_results [i] = i * 2;
            pl.private_sentinels [i] = (int) ((node + 1) * 1000 + i);
        }
        dickpt_generate_ckpt ();
        dickpt_allreduce_ckpt ();
        dickpt_stop_ckpt ();
    }
    for (i = 0; i < N; i ++) {
        if (pl.shared_results [i] != i * 2) {
            if (errors < 10) fprintf (stderr, "[rank %lu] shared_results[%d]=%d want %d\n", node, i, pl.shared_results [i], i * 2);
            errors ++;
        }
        int want_priv = (int) ((node + 1) * 1000 + i);
        if (pl.private_sentinels [i] != want_priv) {
            if (errors < 10) fprintf (stderr, "[rank %lu] private_sentinels[%d]=%d want %d " "(PRIVATE CLOBBERED BY ANOTHER RANK)\n", node, i, pl.
              private_sentinels [i], want_priv);
            errors ++;
        }
    }
    if (errors == 0) {
        printf ("RESULT rank=%lu/%lu status=OK\n", node, nnodes);
        return 0;
    }
    printf ("RESULT rank=%lu/%lu status=FAIL errors=%d\n", node, nnodes, errors);
    return 1;
}

