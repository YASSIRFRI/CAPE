#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int data = 0;

    #pragma omp parallel shared(data)
    {
        #pragma omp master
        {
            data = 42;
            printf("Master set data = %d\n", data);
        }

        #pragma omp barrier

        printf("Thread %d reads data = %d\n", omp_get_thread_num(), data);

        #pragma omp barrier

        #pragma omp master
        {
            printf("All threads done.\n");
        }
    }

    return 0;
}
