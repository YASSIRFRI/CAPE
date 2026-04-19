#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    omp_set_num_threads(4);

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        int nthreads = omp_get_num_threads();
        printf("Hello from thread %d of %d\n", tid, nthreads);
    }

    return 0;
}
