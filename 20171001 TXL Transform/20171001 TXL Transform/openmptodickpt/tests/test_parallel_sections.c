#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int a = 0;
    int b = 0;
    int c = 0;

    #pragma omp parallel sections shared(a, b, c)
    {
        [#pragma omp section]
            a = 1;
            printf("Section 0: a = %d\n", a);

        [#pragma omp section]
            b = 2;
            printf("Section 1: b = %d\n", b);

        [#pragma omp section]
            c = 3;
            printf("Section 2: c = %d\n", c);
    }

    printf("a=%d b=%d c=%d\n", a, b, c);
    return 0;
}
