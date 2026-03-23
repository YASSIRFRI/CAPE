       #include <unistd.h>
       #include <signal.h>
       #include <stdio.h>
       #include <malloc.h>
       #include <stdlib.h>
       #include <errno.h>
       #include <sys/mman.h>
       #include <ucontext.h>
       #define N 10000
       

       #define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

       static char *buffer;
       int pagesize;
       
       int A[N][N];

       static void
       handler(int sig, siginfo_t *si, void *unused)
       {
           printf("Got SIGSEGV at address: 0x%lx\n",
                   (long) si->si_addr);
           mprotect((long)si->si_addr , pagesize, PROT_WRITE ) ;           
          
           
           exit(0);
       }

       int
       main(int argc, char *argv[])
       {
           
           int i,j,k;
           
           
           char * p;
           
           struct sigaction sa;

           sa.sa_flags = SA_SIGINFO;
           sigemptyset(&sa.sa_mask);
           sa.sa_sigaction = handler;
           if (sigaction(SIGSEGV, &sa, NULL) == -1)
               handle_error("sigaction");

           pagesize = sysconf(_SC_PAGE_SIZE);
           if (pagesize == -1)
               handle_error("sysconf");

           /* Allocate a buffer aligned on a page boundary;
              initial protection is PROT_READ | PROT_WRITE */

           buffer = memalign(pagesize, 4 * pagesize);
           if (buffer == NULL)
               handle_error("memalign");

           printf("Start of region:        0x%lx\n", (long) buffer);
           
           long aligned_addr = ((long)&A[0][0]) & ~(pagesize -1);
           printf("After aligned :        0x%lx\n", (long) aligned_addr);
           
           long addr2 = ((long)&A[N-1][N-1]) & ~(pagesize -1);       
           
            
           //lock memory             
           for(i= 1; i< 90000; i++){
					 if (mprotect(aligned_addr + pagesize * i, pagesize,
                       PROT_READ) == -1)
               handle_error("mprotect");
		    }
               
           for(i = 1; i < 10 ; i++ )
				for (j = 0; j < 10 ; j++){
					A[i][j] = i * j ;
			}

          // for (p = buffer ; ; )
           //    *(p++) = 'a';

           printf("Loop completed\n");     /* Should never happen */
           exit(EXIT_SUCCESS);
       }
