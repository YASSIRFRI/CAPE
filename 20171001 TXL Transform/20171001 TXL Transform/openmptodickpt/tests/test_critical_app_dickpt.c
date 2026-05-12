#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>
#define N 1024
#define D 8
static int x [N] [D];
static long long total [D];

int main (int argc, char * * argv) {
    static int x [N] [D];
    static long long total [D];
    int i, j;
    for (i = 0; i < N; i ++) for (j = 0; j < D; j ++) x [i] [j] = (i + j) % 7;
    for (j = 0; j < D; j ++) total [j] = 0;
    dickpt_start_ckpt ();
        long long local [D];
        for (j = 0; j < D; j ++) local [j] = 0;
        {
            unsigned long __dickpt_node__ = dickpt_read_node ();
            unsigned long __dickpt_num_nodes__ = dickpt_read_num_nodes ();
            long long __dickpt_first__ = 0;
            long long __dickpt_last__ = N;
            long long __dickpt_count__ = __dickpt_last__ - __dickpt_first__;
            long long __dickpt_left__ = __dickpt_first__ + (__dickpt_count__ * __dickpt_node__) / __dickpt_num_nodes__;
            long long __dickpt_right__ = __dickpt_first__ + (__dickpt_count__ * (__dickpt_node__ + 1)) / __dickpt_num_nodes__;
            dickpt_send_num_jobs (__dickpt_count__);
            for (i = __dickpt_left__; i < __dickpt_right__; i ++) {
                for (j = 0; j < D; j ++) local [j] += x [i] [j];
            }
        }
        {
            unsigned long __i__;
            for (__i__ = 0; __i__ < dickpt_read_num_nodes (); __i__ ++) {
                if (__i__ == dickpt_read_node ()) {
                    for (j = 0; j < D; j ++) total [j] += local [j];
                }
                dickpt_waitfor_ckpt ();
            }
            }
    dickpt_generate_ckpt ();
    dickpt_allreduce_ckpt ();
    dickpt_stop_ckpt ();
    for (j = 0; j < D; j ++) printf ("total[%d] = %lld\n", j, total [j]);
    return 0;
}

