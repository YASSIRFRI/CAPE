#include <stdio.h>
#include <omp.h>

int main(int argc, char **argv) {
    int flag = 0;
    int data = 0;

    #pragma omp parallel shared(flag, data)
    {
        if (omp_get_thread_num() == 0) {
            data = 42;
            #pragma omp flush
            flag = 1;
            #pragma omp flush
        }
    }

    printf("data = %d, flag = %d\n", data, flag);
    return 0;
}
