#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>
#define N 12
static long input [N];
static long output [N];

static long task_work (long x) {
    long acc = x;
    int k;
    for (k = 0; k < 64; k ++) acc = (acc * 17 + k) % 1000003;
    return acc;
}

int main (int argc, char * * argv) {
    static long input [N];
    static long output [N];
    long checksum = 0;
    long expected = 0;
    int i;
    (void) argc;
    (void) argv;
    for (i = 0; i < N; i ++) {
        input [i] = i + 1;
        output [i] = 0;
        expected += task_work (input [i]);
    }
    dickpt_register_region (& input, sizeof (input));
    dickpt_register_shared (& input, sizeof (input), 1);
    dickpt_register_region (& output, sizeof (output));
    dickpt_register_shared (& output, sizeof (output), 1);
    dickpt_start_ckpt ();
            if (1) {
                dickpt_register_region (& input, sizeof (input));
                dickpt_register_shared (& input, sizeof (input), 2);
                dickpt_register_region (& output, sizeof (output));
                dickpt_register_shared (& output, sizeof (output), 2);

                if (dickpt_read_node () == 0) {
                    dickpt_generate_ckpt ();
                    dickpt_dispatch_task_ckpt ();
                    }
                else if (dickpt_receive_task_ckpt ()) {
                    dickpt_inject_ckpt ();
                    {
                        int j;
                        for (j = 0; j < N / 3; j ++) output [j] = task_work (input [j]);
                    }
                    dickpt_generate_ckpt ();
                    dickpt_send_ckpt ();
                    }
                dickpt_unregister_level (2);

                dickpt_register_region (& input, sizeof (input));
                dickpt_register_shared (& input, sizeof (input), 2);
                dickpt_register_region (& output, sizeof (output));
                dickpt_register_shared (& output, sizeof (output), 2);

                if (dickpt_read_node () == 0) {
                    dickpt_generate_ckpt ();
                    dickpt_dispatch_task_ckpt ();
                    }
                else if (dickpt_receive_task_ckpt ()) {
                    dickpt_inject_ckpt ();
                    {
                        int j;
                        for (j = N / 3; j < (2 * N) / 3; j ++) output [j] = task_work (input [j]);
                    }
                    dickpt_generate_ckpt ();
                    dickpt_send_ckpt ();
                    }
                dickpt_unregister_level (2);

                dickpt_register_region (& input, sizeof (input));
                dickpt_register_shared (& input, sizeof (input), 2);
                dickpt_register_region (& output, sizeof (output));
                dickpt_register_shared (& output, sizeof (output), 2);

                if (dickpt_read_node () == 0) {
                    dickpt_generate_ckpt ();
                    dickpt_dispatch_task_ckpt ();
                    }
                else if (dickpt_receive_task_ckpt ()) {
                    dickpt_inject_ckpt ();
                    {
                        int j;
                        for (j = (2 * N) / 3; j < N; j ++) output [j] = task_work (input [j]);
                    }
                    dickpt_generate_ckpt ();
                    dickpt_send_ckpt ();
                    }
                dickpt_unregister_level (2);

                dickpt_waitfor_ckpt ();
                dickpt_broadcast_ckpt ();
                dickpt_inject_ckpt ();
            }
        dickpt_generate_ckpt ();
        dickpt_allreduce_ckpt ();
        dickpt_stop_ckpt ();
        for (i = 0; i < N; i ++) checksum += output [i];
        printf ("TASK_RESULT checksum=%ld expected=%ld first=%ld last=%ld\n", checksum, expected, output [0], output [N - 1]);
        return checksum == expected ? 0 : 1;
    }

