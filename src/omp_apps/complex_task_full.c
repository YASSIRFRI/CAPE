#include <stdio.h>
#include <stdlib.h>

#define TASK_COUNT 12
#define ITEMS_PER_TASK 16
#define TOTAL_ITEMS (TASK_COUNT * ITEMS_PER_TASK)
#define SCRATCH_WORDS 13
#define MOD_VALUE 1000000007L

static int input_a[TOTAL_ITEMS];
static int input_b[TOTAL_ITEMS];
static long stage1_output[TOTAL_ITEMS];
static long stage1_expected[TOTAL_ITEMS];
static long stage2_output[TOTAL_ITEMS];
static long stage2_expected[TOTAL_ITEMS];
static long task_checksum[TASK_COUNT];
static long task_expected_checksum[TASK_COUNT];
static long stage2_checksum[TASK_COUNT];
static long stage2_expected_checksum[TASK_COUNT];
static int task_errors[TASK_COUNT];
static int current_rep;

static long mod_norm(long value)
{
    value %= MOD_VALUE;
    if (value < 0)
        value += MOD_VALUE;
    return value;
}

static long mix_value(long x, long y, int task_id, int index, int round)
{
    long a = mod_norm(x * (31 + round) + y * (17 + task_id));
    long b = mod_norm((long)(index + 1) * (task_id + 7 + round));
    long c = mod_norm((x + y + current_rep + 3) * (round + 11));

    return mod_norm(a + b + c + (x % (round + 5)));
}

static long stage1_kernel(int a, int b, int task_id, int index)
{
    long x = mod_norm((long)a * 97 + (long)b * 53 + task_id * 211);
    int round;

    for (round = 0; round < 9; round++)
        x = mix_value(x, a + b + round, task_id, index, round);

    return x;
}

static long stage1_reference(int a, int b, int task_id, int index)
{
    long x = mod_norm((long)a * 97 + (long)b * 53 + task_id * 211);
    int round = 0;

    while (round < 9) {
        long a_part = mod_norm(x * (31 + round) + (long)(a + b + round) * (17 + task_id));
        long b_part = mod_norm((long)(index + 1) * (task_id + 7 + round));
        long c_part = mod_norm((x + a + b + round + current_rep + 3) * (round + 11));

        x = mod_norm(a_part + b_part + c_part + (x % (round + 5)));
        round++;
    }

    return x;
}

static long stage2_kernel(long center, long left, long right, int task_id, int index)
{
    long x = mod_norm(center * 19 + left * 7 + right * 13 + task_id * 101);
    int round;

    for (round = 0; round < 7; round++)
        x = mix_value(x, center + left - right + round, task_id + 3, index, round);

    return x;
}

static long stage2_reference(long center, long left, long right, int task_id, int index)
{
    long x = mod_norm(center * 19 + left * 7 + right * 13 + task_id * 101);
    int round = 0;

    while (round < 7) {
        long y = center + left - right + round;
        long a_part = mod_norm(x * (31 + round) + y * (20 + task_id));
        long b_part = mod_norm((long)(index + 1) * (task_id + 10 + round));
        long c_part = mod_norm((x + y + current_rep + 3) * (round + 11));

        x = mod_norm(a_part + b_part + c_part + (x % (round + 5)));
        round++;
    }

    return x;
}

static int verify_private_scratch(int task_id, int stage)
{
    long private_words[SCRATCH_WORDS];
    long seed = mod_norm((task_id + 1) * 1009 + (stage + 1) * 9176 + current_rep * 37);
    int i;
    int errors = 0;

    for (i = 0; i < SCRATCH_WORDS; i++)
        private_words[i] = mod_norm(seed + i * 73 + (i % 3) * 11);

    for (i = 0; i < 64; i++) {
        int slot = (i + task_id + stage) % SCRATCH_WORDS;
        private_words[slot] = mod_norm(private_words[slot] + seed + i * (stage + 5));
        private_words[slot] = mod_norm(private_words[slot] - seed - i * (stage + 5));
    }

    for (i = 0; i < SCRATCH_WORDS; i++) {
        long expected = mod_norm(seed + i * 73 + (i % 3) * 11);
        if (private_words[i] != expected)
            errors++;
    }

    return errors;
}

static void run_stage1_task(int task_id)
{
    int start = task_id * ITEMS_PER_TASK;
    int stop = start + ITEMS_PER_TASK;
    int i;
    int local_errors = verify_private_scratch(task_id, 1);
    long local_sum = 0;
    long local_expected_sum = 0;

    for (i = start; i < stop; i++) {
        long got = stage1_kernel(input_a[i], input_b[i], task_id, i);
        long want = stage1_reference(input_a[i], input_b[i], task_id, i);

        stage1_output[i] = got;
        stage1_expected[i] = want;
        if (got != want)
            local_errors++;

        local_sum = mod_norm(local_sum + got + (long)(i - start + 1) * (task_id + 3));
        local_expected_sum = mod_norm(local_expected_sum + want + (long)(i - start + 1) * (task_id + 3));
    }

    task_errors[task_id] += local_errors;
    task_checksum[task_id] = local_sum;
    task_expected_checksum[task_id] = local_expected_sum;
}

static void run_stage2_task(int task_id)
{
    int start = task_id * ITEMS_PER_TASK;
    int stop = start + ITEMS_PER_TASK;
    int i;
    int local_errors = verify_private_scratch(task_id, 2);
    long local_sum = 0;
    long local_expected_sum = 0;

    for (i = start; i < stop; i++) {
        int left_index = (i == 0) ? (TOTAL_ITEMS - 1) : (i - 1);
        int right_index = (i + 1 == TOTAL_ITEMS) ? 0 : (i + 1);
        long got;
        long want;

        if (stage1_output[i] != stage1_expected[i])
            local_errors++;

        got = stage2_kernel(stage1_output[i], stage1_output[left_index],
                            stage1_output[right_index], task_id, i);
        want = stage2_reference(stage1_expected[i], stage1_expected[left_index],
                                stage1_expected[right_index], task_id, i);

        stage2_output[i] = got;
        stage2_expected[i] = want;
        if (got != want)
            local_errors++;

        local_sum = mod_norm(local_sum + got + (long)(i - start + 1) * (task_id + 5));
        local_expected_sum = mod_norm(local_expected_sum + want + (long)(i - start + 1) * (task_id + 5));
    }

    task_errors[task_id] += local_errors;
    stage2_checksum[task_id] = local_sum;
    stage2_expected_checksum[task_id] = local_expected_sum;
}

static void reset_state(int rep)
{
    int i;

    current_rep = rep;

    for (i = 0; i < TOTAL_ITEMS; i++) {
        input_a[i] = (i * 17 + rep * 23 + 5) % 997;
        input_b[i] = (i * 31 + rep * 19 + 11) % 991;
        stage1_output[i] = -1;
        stage1_expected[i] = -2;
        stage2_output[i] = -3;
        stage2_expected[i] = -4;
    }

    for (i = 0; i < TASK_COUNT; i++) {
        task_checksum[i] = 0;
        task_expected_checksum[i] = 0;
        stage2_checksum[i] = 0;
        stage2_expected_checksum[i] = 0;
        task_errors[i] = 0;
    }
}

static int verify_all(int rep)
{
    long checksum = 0;
    long expected_checksum = 0;
    int errors = 0;
    int i;

    for (i = 0; i < TASK_COUNT; i++) {
        checksum = mod_norm(checksum + task_checksum[i] + stage2_checksum[i]);
        expected_checksum = mod_norm(expected_checksum + task_expected_checksum[i] +
                                     stage2_expected_checksum[i]);
        errors += task_errors[i];
        if (task_checksum[i] != task_expected_checksum[i])
            errors++;
        if (stage2_checksum[i] != stage2_expected_checksum[i])
            errors++;
    }

    for (i = 0; i < TOTAL_ITEMS; i++) {
        if (stage1_output[i] != stage1_expected[i])
            errors++;
        if (stage2_output[i] != stage2_expected[i])
            errors++;
    }

    if (checksum != expected_checksum)
        errors++;
    printf("COMPLEX_TASK_RESULT rep=%d status=%s errors=%d checksum=%ld expected=%ld tasks=%d items=%d\n",
           rep, errors == 0 ? "PASS" : "FAIL", errors, checksum,
           expected_checksum, TASK_COUNT, TOTAL_ITEMS);

    return errors;
}

int main(int argc, char **argv)
{
    int repeats = 1;
    int total_errors = 0;
    int rep;

    if (argc > 1)
        repeats = atoi(argv[1]);
    if (repeats < 1)
        repeats = 1;

    for (rep = 0; rep < repeats; rep++) {
        reset_state(rep);

        #pragma omp parallel shared(input_a, input_b, stage1_output, stage1_expected, stage2_output, stage2_expected, task_checksum, task_expected_checksum, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
        {
            #pragma omp single
            {
                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(0);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(1);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(2);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(3);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(4);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(5);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(6);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(7);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(8);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(9);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(10);
                }

                #pragma omp task shared(input_a, input_b, stage1_output, stage1_expected, task_checksum, task_expected_checksum, task_errors, current_rep)
                {
                    run_stage1_task(11);
                }

                #pragma omp taskwait

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(0);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(1);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(2);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(3);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(4);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(5);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(6);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(7);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(8);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(9);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(10);
                }

                #pragma omp task shared(stage1_output, stage1_expected, stage2_output, stage2_expected, stage2_checksum, stage2_expected_checksum, task_errors, current_rep)
                {
                    run_stage2_task(11);
                }

                #pragma omp taskwait
            }
        }

        total_errors += verify_all(rep);
    }

    printf("COMPLEX_TASK_FINAL status=%s repeats=%d total_errors=%d\n",
           total_errors == 0 ? "PASS" : "FAIL", repeats, total_errors);

    return total_errors == 0 ? 0 : 1;
}
