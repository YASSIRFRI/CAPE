#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>

long long fib (long long n) {
    if (n < 2) {
        return 1;
    }
    return fib (n - 2) + fib (n - 1);
}

int main (int argc, char * * argv) {
    long long n = 0;
    {
        unsigned long __dickpt_node__ = dickpt_read_node ();
        unsigned long __dickpt_num_nodes__ = dickpt_read_num_nodes ();
        long long __dickpt_first__ = 0;
        long long __dickpt_last__ = 45;
        long long __dickpt_count__ = __dickpt_last__ - __dickpt_first__;
        long long __dickpt_left__ = __dickpt_first__ + (__dickpt_count__ * __dickpt_node__) / __dickpt_num_nodes__;
        long long __dickpt_right__ = __dickpt_first__ + (__dickpt_count__ * (__dickpt_node__ + 1)) / __dickpt_num_nodes__;
        dickpt_send_num_jobs (__dickpt_count__);
        dickpt_start_ckpt ();
        for (n = __dickpt_left__; n < __dickpt_right__; n ++) {
            printf ("Fib(%lld): %lld\n", n, fib (n));
        }
        dickpt_generate_ckpt ();
        dickpt_stop_ckpt ();
    }
    return 0;
}

