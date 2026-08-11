#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int rank   = omp_get_thread_num();
    int nranks = omp_get_num_threads();
    int result = rank;          /* captured shared stack variable */

    #pragma omp task shared(result)
    {
        result += 100;
    }

    if (result == rank + 100) {
        printf("RESULT rank=%d/%d status=OK result=%d\n", rank, nranks, result);
        return 0;
    }
    printf("RESULT rank=%d/%d status=FAIL result=%d want=%d\n",
           rank, nranks, result, rank + 100);
    return 1;
}
