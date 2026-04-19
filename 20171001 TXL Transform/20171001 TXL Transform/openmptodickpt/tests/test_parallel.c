#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int x = 10;
    int y = 20;

    #pragma omp parallel shared(x) private(y)
    {
        y = x + omp_get_thread_num();
        printf("Thread %d: y = %d\n", omp_get_thread_num(), y);
    }

    return 0;
}
