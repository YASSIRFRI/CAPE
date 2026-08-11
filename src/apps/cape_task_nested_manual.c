/*
 * Hand-transpiled DICKPT version of a recursive OpenMP task tree:
 *
 *   void traverse(int i) {
 *       result[i] = tree_work(value[i]);
 *       #pragma omp task firstprivate(l) shared(result, subtotal)
 *           traverse(l);
 *       #pragma omp task firstprivate(r) shared(result, subtotal)
 *           traverse(r);
 *       #pragma omp taskwait
 *       subtotal[i] = result[i] + subtotal[l] + subtotal[r];
 *   }
 *
 * Exercises the dynamic task control plane: tasks are spawned from inside
 * tasks running on workers (S_TASK_SPAWN submit path), scheduled by the
 * master, and taskwait inside a task blocks on its direct children (with
 * inline execution of queued tasks while waiting). The subtotal combine
 * after taskwait checks that children's outputs really are injected back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape_dickpt.h"

#define TREE_NODES 15   /* complete binary tree of depth 4 */

static long value[TREE_NODES] __attribute__ ((aligned (4096)));
static long result[TREE_NODES] __attribute__ ((aligned (4096)));
static long subtotal[TREE_NODES] __attribute__ ((aligned (4096)));

static long tree_work(long x)
{
    long acc = x;
    int k;
    for (k = 0; k < 64; k++)
        acc = (acc * 17 + k) % 1000003;
    return acc;
}

static void traverse_task(void *p);

static void traverse(int i)
{
    int l = 2 * i + 1;
    int r = 2 * i + 2;

    result[i] = tree_work(value[i]);

    if (l < TREE_NODES)
        dickpt_task_spawn(traverse_task, &l, sizeof l);
    if (r < TREE_NODES)
        dickpt_task_spawn(traverse_task, &r, sizeof r);

    dickpt_task_wait();

    subtotal[i] = result[i];
    if (l < TREE_NODES)
        subtotal[i] += subtotal[l];
    if (r < TREE_NODES)
        subtotal[i] += subtotal[r];
}

static void traverse_task(void *p)
{
    traverse(*(int *)p);
}

int main(int argc, char **argv)
{
    long expected = 0;
    int i;
    (void)argc;
    (void)argv;

    for (i = 0; i < TREE_NODES; i++) {
        value[i] = i + 1;
        result[i] = 0;
        subtotal[i] = 0;
        expected += tree_work(value[i]);
    }

    dickpt_register_region(&value, sizeof(value));
    dickpt_register_shared(&value, sizeof(value), 1);
    dickpt_register_region(&result, sizeof(result));
    dickpt_register_shared(&result, sizeof(result), 1);
    dickpt_register_region(&subtotal, sizeof(subtotal));
    dickpt_register_shared(&subtotal, sizeof(subtotal), 1);

    dickpt_start_ckpt();

    if (dickpt_read_node() == 0) {
        traverse(0);
        dickpt_task_region_end();
    } else {
        struct dickpt_task_desc d;
        while (dickpt_task_serve(&d)) {
            d.fn(d.args);
            dickpt_task_complete();
        }
    }

    dickpt_broadcast_ckpt();
    dickpt_inject_ckpt();
    dickpt_generate_ckpt();
    dickpt_allreduce_ckpt();
    dickpt_stop_ckpt();

    printf("TASK_NESTED_RESULT subtotal=%ld expected=%ld root=%ld leaf=%ld\n",
           subtotal[0], expected, result[0], result[TREE_NODES - 1]);
    return subtotal[0] == expected ? 0 : 1;
}
