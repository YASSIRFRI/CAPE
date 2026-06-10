#include <stdio.h>
#include <omp.h>

#define TREE_NODES 15

static long value[TREE_NODES];
static long result[TREE_NODES];
static long subtotal[TREE_NODES];

static long tree_work(long x)
{
    long acc = x;
    int k;

    for (k = 0; k < 64; k++)
        acc = (acc * 17 + k) % 1000003;
    return acc;
}

static void traverse(int i)
{
    int l = 2 * i + 1;
    int r = 2 * i + 2;

    result[i] = tree_work(value[i]);

    if (l < TREE_NODES) {
        #pragma omp task shared(result, subtotal) firstprivate(l)
        {
            traverse(l);
        }
    }
    if (r < TREE_NODES) {
        #pragma omp task shared(result, subtotal) firstprivate(r)
        {
            traverse(r);
        }
    }

    #pragma omp taskwait

    subtotal[i] = result[i];
    if (l < TREE_NODES)
        subtotal[i] += subtotal[l];
    if (r < TREE_NODES)
        subtotal[i] += subtotal[r];
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

    #pragma omp parallel shared(value, result, subtotal)
    {
        #pragma omp single
        {
            traverse(0);
        }
    }

    printf("TASK_NESTED_RESULT subtotal=%ld expected=%ld root=%ld leaf=%ld\n",
           subtotal[0], expected, result[0], result[TREE_NODES - 1]);
    return subtotal[0] == expected ? 0 : 1;
}
