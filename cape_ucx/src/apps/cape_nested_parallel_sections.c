#include <stdio.h>
#include <stdlib.h>
#include "../../include/cape.h"

#define NUM_SECTIONS 6

void *__get_pc(void)
{
	return __builtin_return_address(0);
}

static int section_owner[NUM_SECTIONS];
static int section_value[NUM_SECTIONS];

static int tid;
static int i;
static int nnodes;
static int errors;
static int expected_owner;
static int expected_value;

#define RUN_SECTION(IDX, BASE)                    \
	do {                                      \
		if (cape_section()) {             \
			section_owner[(IDX)] = tid;   \
			section_value[(IDX)] = (BASE) + tid; \
		}                                 \
	} while (0)

int main(void)
{
	cape_declare_variable(&section_owner, CAPE_INT, NUM_SECTIONS, 0);
	cape_declare_variable(&section_value, CAPE_INT, NUM_SECTIONS, 0);
	cape_declare_variable(&tid, CAPE_INT, 1, 0);
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&nnodes, CAPE_INT, 1, 0);
	cape_declare_variable(&errors, CAPE_INT, 1, 0);
	cape_declare_variable(&expected_owner, CAPE_INT, 1, 0);
	cape_declare_variable(&expected_value, CAPE_INT, 1, 0);

	cape_init();

	nnodes = cape_get_num_nodes();
	errors = 0;
	for (i = 0; i < NUM_SECTIONS; i++) {
		section_owner[i] = -1;
		section_value[i] = -1;
	}

	cape_begin(PARALLEL, 0, 0);
	cape_set_private(&tid);
	cape_set_private(&i);
	cape_set_private(&expected_owner);
	cape_set_private(&expected_value);
	cape_set_shared(&section_owner);
	cape_set_shared(&section_value);
	cape_set_shared(&nnodes);
	cape_set_shared(&errors);
	ckpt_start();
	{
		tid = cape_get_node_num();

		cape_begin(SECTIONS, 0, 0);
		{
			RUN_SECTION(0, 1000);
			RUN_SECTION(1, 2000);
			RUN_SECTION(2, 3000);
			RUN_SECTION(3, 4000);
			RUN_SECTION(4, 5000);
			RUN_SECTION(5, 6000);
		}
		__pc__ = (unsigned long)__get_pc();
		cape_end(SECTIONS, FALSE);
	}
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL, FALSE);

	if (cape_get_node_num() == 0) {
		for (i = 0; i < NUM_SECTIONS; i++) {
			expected_owner = i % nnodes;
			expected_value = (i + 1) * 1000 + expected_owner;
			if (section_owner[i] != expected_owner || section_value[i] != expected_value) {
				errors++;
				printf("FAIL section=%d owner=%d expected_owner=%d value=%d expected_value=%d\n",
				       i, section_owner[i], expected_owner,
				       section_value[i], expected_value);
			}
		}

		if (errors == 0) {
			printf("PASS nested_parallel_sections nodes=%d sections=%d\n",
			       nnodes, NUM_SECTIONS);
		} else {
			printf("FAIL nested_parallel_sections errors=%d nodes=%d sections=%d\n",
			       errors, nnodes, NUM_SECTIONS);
		}
	}

	cape_finalize();
	return errors == 0 ? 0 : 1;
}
