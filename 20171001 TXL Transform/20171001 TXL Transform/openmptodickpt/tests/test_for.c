#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int i;
    double result = 0.0;

    #pragma omp parallel shared(result)
    {
        #pragma omp for private(i) reduction(+:result)
        for (i = 1; i < 1000; i++) {
            result += 1.0 / i;
        }
    }

    printf("Result = %f\n", result);
    return 0;
}
