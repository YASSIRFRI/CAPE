       #include <unistd.h>
       #include <signal.h>
       #include <stdio.h>
       #include <malloc.h>
       #include <stdlib.h>
       #include <errno.h>
       #include <sys/mman.h>
       #include <ucontext.h>
       #define N 100     

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
          
           //TODO: Remove Write protect a page and resume execution at the point that SIGSEGV occurs
           
           exit(0);
       }

       int
       main(int argc, char *argv[])
       {           
           int i,j,k;                  
       
           struct sigaction sa;

           sa.sa_flags = SA_SIGINFO;
           sigemptyset(&sa.sa_mask);
           sa.sa_sigaction = handler;
           if (sigaction(SIGSEGV, &sa, NULL) == -1)
               handle_error("sigaction");

           pagesize = sysconf(_SC_PAGE_SIZE);
            
           long aligned_addr = ((long)&A[0][0]) & ~(pagesize -1);
           
           //lock memory             
           for(i= 1; i< N; i++){
					 if (mprotect(aligned_addr + pagesize * i, pagesize,
                       PROT_READ) == -1)
               handle_error("mprotect");
		    }
               
           for(i = 1; i < 10 ; i++ )
				for (j = 0; j < 10 ; j++){
					A[i][j] = i * j ;
			}

           printf("Loop completed\n");     /* Should never happen */
           exit(EXIT_SUCCESS);
       }
