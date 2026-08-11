#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>

#define handle_error(msg) \
    do { perror(msg); exit(EXIT_FAILURE); } while (0)

char *buffer;

static void
handler(int sig, siginfo_t *si, void *unused)
{
    printf("Reçu SIGSEGV à l'adresse : 0x%lx\n", 
            (long) si->si_addr);
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
    char *p;
    int pagesize;
    struct sigaction sa;

    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1)
        handle_error("sigaction");

    pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == -1)
        handle_error("sysconf");

    /* Alloue un tampon aligné sur une frontière de page ;
       la protection initiale est PROT_READ | PROT_WRITE */

    buffer = memalign(pagesize, 4 * pagesize);
    if (buffer == NULL)
        handle_error("memalign");

    printf("Début de la région :       0x%lx\n", (long) buffer);

    if (mprotect(buffer + pagesize * 2, pagesize,
                PROT_NONE) == -1)
        handle_error("mprotect");

    for (p = buffer ; ; )
        *(p++) = aqaaq;

    printf("Boucle terminée\n");     /* Ne devrait jamais arriver */
    exit(EXIT_SUCCESS);
}
