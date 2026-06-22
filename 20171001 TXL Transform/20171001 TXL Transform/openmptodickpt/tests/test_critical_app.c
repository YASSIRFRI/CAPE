/* Minimal critical-section app, inspired by omp_gradient.c's
 * accumulate-into-shared-array pattern.  Each thread/node computes
 * a local partial vector and then folds it into the global one
 * inside a critical region. */

#include <stdio.h>
#include <omp.h>

#define N 1024
#define D 8

static int x[N][D];
static long long total[D];

int main(int argc, char **argv) {
    int i, j;

    for (i = 0; i < N; i++)
        for (j = 0; j < D; j++)
            x[i][j] = (i + j) % 7;

    for (j = 0; j < D; j++)
        total[j] = 0;

    #pragma omp parallel
    {
        long long local[D];

        for (j = 0; j < D; j++)
            local[j] = 0;

        #pragma omp for
        for (i = 0; i < N; i++) {
            for (j = 0; j < D; j++)
                local[j] += x[i][j];
        }

        #pragma omp critical
        {
            for (j = 0; j < D; j++)
                total[j] += local[j];
        }
    }

    {
        long long expected[D];
        int ok = 1;

        for (j = 0; j < D; j++)
            expected[j] = 0;
        for (i = 0; i < N; i++)
            for (j = 0; j < D; j++)
                expected[j] += x[i][j];
        for (j = 0; j < D; j++) {
            if (total[j] != expected[j])
                ok = 0;
            printf("total[%d] = %lld expected %lld\n", j, total[j], expected[j]);
        }
        printf("CRITICAL_RESULT ok=%d\n", ok);
        return ok ? 0 : 1;
    }
}
