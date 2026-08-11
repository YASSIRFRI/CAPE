#include <stdio.h>
#include <stdlib.h>
#include <omp.h>

#define N 64

int a[N][N];
int b[N][N];
int c[N][N];

int main(int argc, char **argv) {
    int i, j, k;

    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            a[i][j] = rand() % 10;
            b[i][j] = rand() % 10;
            c[i][j] = 0;
        }
    }

    #pragma omp parallel for private(i, j, k) shared(a, b, c)
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            for (k = 0; k < N; k++) {
                c[i][j] += a[i][k] * b[k][j];
            }
        }
    }

    printf("c[0][0] = %d\n", c[0][0]);
    printf("c[%d][%d] = %d\n", N - 1, N - 1, c[N - 1][N - 1]);
    return 0;
}
