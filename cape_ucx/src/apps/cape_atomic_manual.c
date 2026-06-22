#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>
#define N 64
static long data [N];
static long total;

int main (int argc, char * * argv) {
    static long data [N] __attribute__ ((aligned (4096)));
    static long total __attribute__ ((aligned (4096)));
    long expected = 0;
    int i;
    (void) argc;
    (void) argv;
    for (i = 0; i < N; i ++) {
        data [i] = 3 * i + 1;
        expected += data [i];
    }
    total = 0;
    dickpt_register_region (& data, sizeof (data));
    dickpt_register_shared (& data, sizeof (data), 1);
    dickpt_register_region (& total, sizeof (total));
    dickpt_register_shared (& total, sizeof (total), 1);
    dickpt_start_ckpt ();
            long local = 0;
            {
                unsigned long __dickpt_node__ = dickpt_read_node ();
                unsigned long __dickpt_num_nodes__ = dickpt_read_num_nodes ();
                long long __dickpt_first__ = 0;
                long long __dickpt_last__ = N;
                long long __dickpt_count__ = __dickpt_last__ - __dickpt_first__;
                long long __dickpt_left__ = __dickpt_first__ + (__dickpt_count__ * __dickpt_node__) / __dickpt_num_nodes__;
                long long __dickpt_right__ = __dickpt_first__ + (__dickpt_count__ * (__dickpt_node__ + 1)) / __dickpt_num_nodes__;
                dickpt_send_num_jobs (__dickpt_count__);
                for (i = __dickpt_left__; i < __dickpt_right__; i ++) local += data [i];
            }
            dickpt_critical_enter ();
            total += local;
            dickpt_critical_exit ();
        dickpt_generate_ckpt ();
        dickpt_allreduce_ckpt ();
        dickpt_stop_ckpt ();
        printf ("ATOMIC_RESULT total=%ld expected=%ld\n", total, expected);
        return total == expected ? 0 : 1;
    }

