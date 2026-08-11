#include <stdio.h>
#include <omp.h>

#define N 12

static long input[N];
static long output[N];

static long task_work(long x)
{
    long acc = x;
    int k;

    for (k = 0; k < 64; k++)
        acc = (acc * 17 + k) % 1000003;
    return acc;
}

int main(int argc, char **argv)
{
    long checksum = 0;
    long expected = 0;
    int i;

    (void)argc;
    (void)argv;

    for (i = 0; i < N; i++) {
        input[i] = i + 1;
        output[i] = 0;
        expected += task_work(input[i]);
    }

    #pragma omp parallel shared(input, output)
    {
        #pragma omp single
        {
            #pragma omp task shared(input, output)
            {
                int j;
                for (j = 0; j < N / 3; j++)
                    output[j] = task_work(input[j]);
            }

            #pragma omp task shared(input, output)
            {
                int j;
                for (j = N / 3; j < (2 * N) / 3; j++)
                    output[j] = task_work(input[j]);
            }

            #pragma omp task shared(input, output)
            {
                int j;
                for (j = (2 * N) / 3; j < N; j++)
                    output[j] = task_work(input[j]);
            }

            #pragma omp taskwait
        }
    }

    for (i = 0; i < N; i++)
        checksum += output[i];

    printf("TASK_RESULT checksum=%ld expected=%ld first=%ld last=%ld\n",
           checksum, expected, output[0], output[N - 1]);
    return checksum == expected ? 0 : 1;
}
