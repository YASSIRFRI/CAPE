#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int x = 0;
    int y = 0;

    #pragma omp parallel shared(x, y)
    {
        #pragma omp sections
        {
            [#pragma omp section]
                x = 10;
                printf("Section 0 set x = %d\n", x);

            [#pragma omp section]
                y = 20;
                printf("Section 1 set y = %d\n", y);
        }
    }

    printf("x = %d, y = %d\n", x, y);
    return 0;
}
