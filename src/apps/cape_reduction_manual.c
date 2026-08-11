/* Manual reduction test for the bitmap monitor.
 *
 * Each rank sums its slice of a globally-defined array into a scalar
 * `sum` (double) and a scalar `count` (int). Both are declared as
 * reduction variables to the monitor before the checkpoint window.
 * After dickpt_allreduce_ckpt, every rank must hold the global sum
 * and the global element count.
 *
 * Total expected sum: each a[i] = (double)(i+1), so sum_{i=0..N-1} (i+1)
 *                    = N*(N+1)/2.
 * Total expected count: N. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../include/cape_dickpt.h"

#define DEFAULT_N 100000

static double a[DEFAULT_N];

struct ckpt_state {
    double sum;
    int    count;
    char   _pad[4096 - sizeof(double) - sizeof(int)];
};
static struct ckpt_state g_state __attribute__((aligned(4096)));

int main(int argc, char *argv[])
{
    int n = DEFAULT_N;
    int reps = 1, rep;
    unsigned long node, num_nodes;
    int row_start, row_end, i;

    if (argc > 1) reps = atoi(argv[1]);
    if (reps <= 0) reps = 1;

    memset(&g_state, 0, sizeof(g_state));
    dickpt_register_region(&g_state, sizeof(g_state));

    /* Tell the monitor: g_state.sum is a double SUM reduction,
     *                   g_state.count is an int  SUM reduction. */
    dickpt_declare_reduction(&g_state.sum,   CAPE_DOUBLE, D_REDUCTION_SUM);
    dickpt_declare_reduction(&g_state.count, CAPE_INT,    D_REDUCTION_SUM);

    node = dickpt_read_node();
    num_nodes = dickpt_read_num_nodes();
    dickpt_send_num_jobs(n);

    for (i = 0; i < n; i++)
        a[i] = (double)(i + 1);

    row_start = (n * (int)node) / (int)num_nodes;
    row_end   = (n * ((int)node + 1)) / (int)num_nodes;

    for (rep = 1; rep <= reps; rep++) {
        double local_sum = 0.0;
        int    local_count = 0;
        double expected_sum = (double)n * ((double)n + 1.0) / 2.0;

        g_state.sum = 0.0;
        g_state.count = 0;

        dickpt_start_ckpt();

        for (i = row_start; i < row_end; i++) {
            local_sum += a[i];
            local_count++;
        }
        g_state.sum   = local_sum;
        g_state.count = local_count;

        dickpt_generate_ckpt();
        dickpt_allreduce_ckpt();
        dickpt_stop_ckpt();

        if (g_state.count != n ||
            fabs(g_state.sum - expected_sum) > 1e-6) {
            fprintf(stderr,
                "VERIFY FAIL node=%lu rep=%d sum=%.6f (expected %.6f) "
                "count=%d (expected %d)\n",
                node, rep, g_state.sum, expected_sum,
                g_state.count, n);
            return 1;
        }

        if (node == 0)
            printf("VERIFY OK rep=%d sum=%.6f count=%d (n=%d, ranks=%lu)\n",
                   rep, g_state.sum, g_state.count, n, num_nodes);
    }

    return 0;
}
