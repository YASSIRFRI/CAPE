#include <stdio.h>

struct stack_frame {
        struct stack_frame *prev;
        void *return_addr;
} __attribute__((packed));
typedef struct stack_frame stack_frame;

void backtrace_from_fp(void **buf, int size)
{
        int i;
        stack_frame *fp;

        __asm__("movl %%ebp, %[fp]" :  /* output */ [fp] "=r" (fp));

        for(i = 0; i < size && fp != NULL; fp = fp->prev, i++)
                buf[i] = fp->return_addr;
}

void main{



}
