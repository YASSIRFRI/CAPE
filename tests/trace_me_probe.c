 #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include <errno.h>
  #include <unistd.h>
  #include <sys/ptrace.h>
  #include <sys/prctl.h>
  #include <sys/wait.h>
  int main(void) {
      FILE *f = fopen("/proc/sys/kernel/yama/ptrace_scope","r");
      int scope=-1; if(f){fscanf(f,"%d",&scope); fclose(f);}
      pid_t pid = fork();
      if (pid == 0) {
          prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
          long r = ptrace(PTRACE_TRACEME, 0, 0, 0);
          fprintf(stderr, "child: TRACEME rc=%ld errno=%s scope=%d\n",
                  r, strerror(errno), scope);
          _exit(r == 0 ? 0 : 42);
      }
      int st; waitpid(pid, &st, 0);
      fprintf(stderr, "parent: child exit=0x%x WIFEXITED=%d code=%d\n",
              st, WIFEXITED(st), WEXITSTATUS(st));
      return 0;
  }