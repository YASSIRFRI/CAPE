#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../cape_ucx/include/cape.h"
#include <stdio.h>

int main (int argc, char * * argv) {
    cape_init ();
    int rank = cape_get_node_num ();
    int nranks = cape_get_num_nodes ();
    int result = rank;
    {
        struct {__typeof__ (result) result;
        } * __cape_env = cape_task_env_alloc (sizeof (* __cape_env));
        __cape_env -> result = result;
            __cape_env -> result += 100;
        result = __cape_env -> result;
    }
    if (result == rank + 100) {
        printf ("RESULT rank=%d/%d status=OK result=%d\n", rank, nranks, result);
        return 0;
    }
    printf ("RESULT rank=%d/%d status=FAIL result=%d want=%d\n", rank, nranks, result, rank + 100);
    return 1;
    cape_finalize ();
}

