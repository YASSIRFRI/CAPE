#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int i;
    int sum = 0;

    #pragma omp parallel for private(i) reduction(+:sum)
    for (i = 0; i < 100; i++) {
        sum += i;
    }

    printf("Sum = %d\n", sum);
    return 0;
}
