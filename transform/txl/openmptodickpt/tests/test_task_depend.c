#include <stdio.h>
#include <omp.h>

/*
 * OpenMP task dependency test.
 *
 * A diamond DAG over four single-cell buffers:
 *
 *        A (out:a)
 *       /          \
 *   B (in:a,out:b)  C (in:a,out:c)
 *       \          /
 *        D (in:b,c, out:d)
 *
 * Each task writes  child = parent(s) + K, where K is a per-task constant.
 * The chains are arranged so that ANY out-of-order execution makes a task
 * read a still-zero input and the final value diverges from the expected
 * sum of constants. A scheduler that ignores depend() will (almost always)
 * produce the wrong answer; a correct one always yields expected.
 *
 * a = 0 + 10            = 10
 * b = a + 20            = 30
 * c = a + 30            = 40
 * d = b + c + 40        = 110
 */

#define KA 10
#define KB 20
#define KC 30
#define KD 40

static long a;
static long b;
static long c;
static long d;

/* A little arithmetic so the work is not optimised to nothing. */
static long busy(long base, int spins)
{
    long acc = base;
    int k;
    for (k = 0; k < spins; k++)
        acc = acc + ((base ^ k) & 1);
    return base + (acc - base) * 0;   /* == base, but the compiler can't tell */
}

int main(int argc, char **argv)
{
    long expected_a = KA;
    long expected_b = KA + KB;
    long expected_c = KA + KC;
    long expected_d = (KA + KB) + (KA + KC) + KD;
    int ok;

    (void)argc;
    (void)argv;

    a = b = c = d = 0;

    #pragma omp parallel shared(a, b, c, d)
    {
        #pragma omp single
        {
            #pragma omp task depend(out: a) shared(a)
            {
                a = busy(0, 4096) + KA;
            }

            #pragma omp task depend(in: a) depend(out: b) shared(a, b)
            {
                b = busy(a, 4096) + KB;
            }

            #pragma omp task depend(in: a) depend(out: c) shared(a, c)
            {
                c = busy(a, 4096) + KC;
            }

            #pragma omp task depend(in: b, c) depend(out: d) shared(b, c, d)
            {
                d = busy(b + c, 4096) + KD;
            }

            #pragma omp taskwait
        }
    }

    ok = (a == expected_a) && (b == expected_b) &&
         (c == expected_c) && (d == expected_d);

    printf("DEPEND_RESULT a=%ld/%ld b=%ld/%ld c=%ld/%ld d=%ld/%ld status=%s\n",
           a, expected_a, b, expected_b, c, expected_c, d, expected_d,
           ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
