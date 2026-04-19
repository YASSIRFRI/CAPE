#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int i;
    int sum = 0;
    int product = 1;

    #pragma omp parallel for reduction(+:sum)
    for (i = 1; i < 11; i++) {
        sum += i;
    }

    #pragma omp parallel for reduction(*:product)
    for (i = 1; i < 6; i++) {
        product *= i;
    }

    printf("Sum(1..10) = %d\n", sum);
    printf("Product(1..5) = %d\n", product);
    return 0;
}
