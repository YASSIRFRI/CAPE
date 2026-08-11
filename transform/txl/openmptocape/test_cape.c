#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <malloc.h>
#include <errno.h>
#include "../../include/cape.h"
#include "mpi.h"
#include <stdio.h>

long long fib (long long n) {
    if (n < 2) {
        return 1;
    }
    return fib (n - 2) + fib (n - 1);
}

int main (int argc, char * * argv) {
    CAPE_Init ();
    long long n = 0;
    CAPE_Begin (PARALLEL_FOR, 0, 45);
    for (i = __left__; i < __right__; i ++) {
        printf ("Fib(%lld): %lld\n", n, fib (n));
    }
    CAPE_End (PARALLEL_FOR, i, FALSE);
    return 0;
    CAPE_Finalize ();
}

