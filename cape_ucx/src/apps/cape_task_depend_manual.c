#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"
#include <stdio.h>
#define KA 10
#define KB 20
#define KC 30
#define KD 40
static long a;
static long b;
static long c;
static long d;

static long busy (long base, int spins) {
    long acc = base;
    int k;
    for (k = 0; k < spins; k ++) acc = acc + ((base ^ k) & 1);
    return base + (acc - base) * 0;
}

int main (int argc, char * * argv) {
    static long a __attribute__ ((aligned (4096)));
    static long b __attribute__ ((aligned (4096)));
    static long c __attribute__ ((aligned (4096)));
    static long d __attribute__ ((aligned (4096)));
    long expected_a = KA;
    long expected_b = KA + KB;
    long expected_c = KA + KC;
    long expected_d = (KA + KB) + (KA + KC) + KD;
    int ok;
    (void) argc;
    (void) argv;
    a = b = c = d = 0;
    dickpt_register_region (& a, sizeof (a));
    dickpt_register_shared (& a, sizeof (a), 1);
    dickpt_register_region (& b, sizeof (b));
    dickpt_register_shared (& b, sizeof (b), 1);
    dickpt_register_region (& c, sizeof (c));
    dickpt_register_shared (& c, sizeof (c), 1);
    dickpt_register_region (& d, sizeof (d));
    dickpt_register_shared (& d, sizeof (d), 1);
    dickpt_start_ckpt ();
            if (1) {
                dickpt_register_region (& a, sizeof (a));
                dickpt_register_shared (& a, sizeof (a), 2);
                dickpt_task_depend (& a, CAPE_DEP_OUT);

                if (dickpt_read_node () == 0) {
                    dickpt_generate_ckpt ();
                    dickpt_dispatch_task_ckpt ();
                    }
                else if (dickpt_receive_task_ckpt ()) {
                    dickpt_inject_ckpt ();
                    {
                        a = busy (0, 4096) + KA;
                    }
                    dickpt_generate_ckpt ();
                    dickpt_send_ckpt ();
                    }
                dickpt_unregister_level (2);

                dickpt_register_region (& a, sizeof (a));
                dickpt_register_shared (& a, sizeof (a), 2);
                dickpt_register_region (& b, sizeof (b));
                dickpt_register_shared (& b, sizeof (b), 2);
                dickpt_task_depend (& a, CAPE_DEP_IN);
                dickpt_task_depend (& b, CAPE_DEP_OUT);

                if (dickpt_read_node () == 0) {
                    dickpt_generate_ckpt ();
                    dickpt_dispatch_task_ckpt ();
                    }
                else if (dickpt_receive_task_ckpt ()) {
                    dickpt_inject_ckpt ();
                    {
                        b = busy (a, 4096) + KB;
                    }
                    dickpt_generate_ckpt ();
                    dickpt_send_ckpt ();
                    }
                dickpt_unregister_level (2);

                dickpt_register_region (& a, sizeof (a));
                dickpt_register_shared (& a, sizeof (a), 2);
                dickpt_register_region (& c, sizeof (c));
                dickpt_register_shared (& c, sizeof (c), 2);
                dickpt_task_depend (& a, CAPE_DEP_IN);
                dickpt_task_depend (& c, CAPE_DEP_OUT);

                if (dickpt_read_node () == 0) {
                    dickpt_generate_ckpt ();
                    dickpt_dispatch_task_ckpt ();
                    }
                else if (dickpt_receive_task_ckpt ()) {
                    dickpt_inject_ckpt ();
                    {
                        c = busy (a, 4096) + KC;
                    }
                    dickpt_generate_ckpt ();
                    dickpt_send_ckpt ();
                    }
                dickpt_unregister_level (2);

                dickpt_register_region (& b, sizeof (b));
                dickpt_register_shared (& b, sizeof (b), 2);
                dickpt_register_region (& c, sizeof (c));
                dickpt_register_shared (& c, sizeof (c), 2);
                dickpt_register_region (& d, sizeof (d));
                dickpt_register_shared (& d, sizeof (d), 2);
                dickpt_task_depend (& b, CAPE_DEP_IN);
                dickpt_task_depend (& c, CAPE_DEP_IN);
                dickpt_task_depend (& d, CAPE_DEP_OUT);

                if (dickpt_read_node () == 0) {
                    dickpt_generate_ckpt ();
                    dickpt_dispatch_task_ckpt ();
                    }
                else if (dickpt_receive_task_ckpt ()) {
                    dickpt_inject_ckpt ();
                    {
                        d = busy (b + c, 4096) + KD;
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
        ok = (a == expected_a) && (b == expected_b) && (c == expected_c) && (d == expected_d);
        printf ("DEPEND_RESULT a=%ld/%ld b=%ld/%ld c=%ld/%ld d=%ld/%ld status=%s\n", a, expected_a, b, expected_b, c, expected_c, d, expected_d, ok ? "OK" :
          "FAIL");
        return ok ? 0 : 1;
    }

