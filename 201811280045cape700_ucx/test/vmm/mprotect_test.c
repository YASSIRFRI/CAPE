#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define N 100

static int alloc_size;
static char* memory;

int A[N][N];

void segv_handler (int sig, siginfo_t *si, void *unused)
{
 printf("Got SIGSEGV at address: 0x%lx\n", (long) si->si_addr);    
 mprotect (memory, alloc_size, PROT_READ | PROT_WRITE);
} 

int main ()
{
 int fd;
 struct sigaction sa;

 /* Install segv_handler as the handler for SIGSEGV. */
 sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = segv_handler;
	sigaction (SIGSEGV, &sa, NULL);
 
// memset (&sa, 0, sizeof (sa));
// sa.sa_handler = &segv_handler;
// sigaction (SIGSEGV, &sa, NULL);
 
 /* Allocate one page of memory by mapping /dev/zero. Map the memory
   as write-only, initially. */
 alloc_size = getpagesize ();
 fd = open ("/dev/zero", O_RDONLY);
 memory = mmap (NULL, alloc_size, PROT_WRITE, MAP_PRIVATE, fd, 0);
 close (fd);
 /* Write to the page to obtain a private copy. */
 //memory[0] = 0;
 A[0][0] = 1;
 /* Make the memory unwritable. */
 mprotect (memory, alloc_size, PROT_NONE);

 /* Write to the allocated memory region. */
 A[1][1] = 10 ;
 
 //memory[0] = 1;

 /* All done; unmap the memory. */
 printf ("all done\n");
 munmap (memory, alloc_size);
 return 0;
}
