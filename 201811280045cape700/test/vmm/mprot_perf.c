#include <sys/mman.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>

#define NS_IN_SEC 1000000000L
#define PAGE_SIZE (4*1024)
#define START_ADDRESS 0x8000000
#define N_ITERATIONS 10000

void *addr;

static void handler(int sig, siginfo_t *si, void *unused)
{
//    printf("Got SIGSEGV at address: 0x%lx\n",(long) si->si_addr);
    addr = mmap((void*)0x8000000, 1024*4, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_FIXED | MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
    //mprotect(addr, 1024*4, PROT_READ|PROT_WRITE|PROT_EXEC);
}

void setup_handler()
{
    struct sigaction sa;

    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1)
        exit(-1);
}

unsigned long long int get_time()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*NS_IN_SEC+ts.tv_nsec;
}

int main()
{
    setup_handler();

    addr = mmap((void*)START_ADDRESS, PAGE_SIZE, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_FIXED | MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);

    if( mprotect(addr, PAGE_SIZE, PROT_NONE) != 0) {
        puts("Error mprotect");
        exit(-1);
    }

    if (munmap(addr, PAGE_SIZE) != 0) {
        puts("munmap error");
        exit(-1);
    }

    unsigned long long total = 0;

    int i;
    for (i = 0; i < N_ITERATIONS; ++i) {
        unsigned long long start = get_time();
        *(int*)addr = 3;
        unsigned long long end = get_time();
        if( mprotect(addr, PAGE_SIZE, PROT_NONE) != 0) {
            puts("Error mprotect");
            exit(-1);
        }
        total += end-start;
    }

    printf("Average time spent: %lldns\n", total/N_ITERATIONS);

    return 0;
}
