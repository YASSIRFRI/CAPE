#include <stdio.h>
#include <omp.h>

#define N 64

static long data[N];
static long total;

int main(int argc, char **argv)
{
    long expected = 0;
    int i;

    (void)argc;
    (void)argv;

    for (i = 0; i < N; i++) {
        data[i] = 3 * i + 1;
        expected += data[i];
    }
    total = 0;

    #pragma omp parallel shared(data, total)
    {
        long local = 0;

        #pragma omp for
        for (i = 0; i < N; i++)
            local += data[i];

        #pragma omp atomic
        total += local;
    }

    printf("ATOMIC_RESULT total=%ld expected=%ld\n", total, expected);
    return total == expected ? 0 : 1;
}
