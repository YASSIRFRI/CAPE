#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int count = 0;

    #pragma omp parallel shared(count)
    {
        #pragma omp master
        {
            count = omp_get_num_threads();
            printf("Master: total threads = %d\n", count);
        }

        #pragma omp barrier

        printf("Thread %d: count = %d\n", omp_get_thread_num(), count);
    }

    return 0;
}
