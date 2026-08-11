#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int value = 0;

    #pragma omp parallel shared(value)
    {
        #pragma omp single
        {
            value = 42;
            printf("Single: value set to %d by thread %d\n", value, omp_get_thread_num());
        }

        printf("Thread %d sees value = %d\n", omp_get_thread_num(), value);
    }

    return 0;
}
