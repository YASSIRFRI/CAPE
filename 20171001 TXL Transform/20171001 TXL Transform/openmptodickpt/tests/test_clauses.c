#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int a = 100;
    int b = 200;
    int c = 0;
    int i;

    #pragma omp parallel default(none) shared(a, b) private(c) firstprivate(i)
    {
        c = a + b + omp_get_thread_num();
        printf("Thread %d: c = %d, i = %d\n", omp_get_thread_num(), c, i);
    }

    printf("After parallel: a = %d, b = %d\n", a, b);
    return 0;
}
