       #include <unistd.h>
       #include <signal.h>
       #include <stdio.h>
       #include <malloc.h>
       #include <stdlib.h>
       #include <errno.h>
       #include <sys/mman.h>
       #include <ucontext.h>
       #define N 10
       

       #define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

       int pagesize;
       
       int A[N][N];

       static void
       handler(int sig, siginfo_t *si, void *unused)
       {
		   ucontext_t *u = (ucontext_t *)unused;

           printf("Got SIGSEGV at address: 0x%lx\n",
                   (long) si->si_addr);
           mprotect((long)si->si_addr , pagesize, PROT_WRITE | PROT_READ | PROT_EXEC ) ;
           
			unsigned char *pc = (unsigned char *)u->uc_mcontext.gregs[14];
       
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
           if (pagesize == -1)
               handle_error("sysconf");
               
            for(i = 0; i < N ; i++ )
				for (j = 0; j < N ; j++){
					A[i][j] =0;
		   }
           /* Allocate a buffer aligned on a page boundary;
              initial protection is PROT_READ | PROT_WRITE */

       
           long aligned_addr = ((long)&A[0][0]) & ~(pagesize -1);
           printf("After aligned :        0x%lx\n", (long) aligned_addr);
           
           long addr2 = ((long)&A[N-1][N-1]) & ~(pagesize -1); 
           
           while( aligned_addr <= addr2){
				 if (mprotect(aligned_addr, pagesize, PROT_READ) == -1) handle_error("mprotect");
				 aligned_addr+= pagesize;
		   }      
           
            
              
           for(i = 0; i < N ; i++ )
				for (j = 0; j < N ; j++){
					A[i][j] = i * j ;
		   }

         
           printf("Loop completed\n");     /* Should never happen */
           exit(EXIT_SUCCESS);
       }
