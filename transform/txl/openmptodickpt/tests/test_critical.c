#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int total = 0;

    #pragma omp parallel shared(total)
    {
        int local = omp_get_thread_num() + 1;

        #pragma omp critical
        {
            total += local;
            printf("Thread %d added %d, total = %d\n", omp_get_thread_num(), local, total);
        }
    }

    printf("Final total = %d\n", total);
    return 0;
}
