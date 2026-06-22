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
    static int x [N] [D] __attribute__ ((aligned (4096)));
    static long long total [D] __attribute__ ((aligned (4096)));
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
            dickpt_critical_enter ();
            {
                for (j = 0; j < D; j ++) total [j] += local [j];
            }
            dickpt_critical_exit ();
        dickpt_generate_ckpt ();
        dickpt_allreduce_ckpt ();
        dickpt_stop_ckpt ();
        {
            long long expected [D];
            int ok = 1;
            for (j = 0; j < D; j ++) expected [j] = 0;
            for (i = 0; i < N; i ++) for (j = 0; j < D; j ++) expected [j] += x [i] [j];
            for (j = 0; j < D; j ++) {
                if (total [j] != expected [j]) ok = 0;
                printf ("total[%d] = %lld expected %lld\n", j, total [j], expected [j]);
            }
            printf ("CRITICAL_RESULT ok=%d\n", ok);
            return ok ? 0 : 1;
        }
    }

