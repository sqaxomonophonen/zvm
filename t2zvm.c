#include <stdlib.h>
#include <stdio.h>

#include "zvm.h"

uint32_t module_id_and;
uint32_t module_id_or;
uint32_t module_id_not;

static uint32_t emit_and()
{
	zvm_begin_module(2, 1);
	struct zvm_pi i0 = zvm_op_input(0);
	struct zvm_pi i1 = zvm_op_input(1);
	zvm_op_output(0, zvm_op_nor(zvm_op_nor(i0, i0), zvm_op_nor(i1, i1)));
	return zvm_end_module();
}

static uint32_t emit_or()
{
	zvm_begin_module(2, 1);
	struct zvm_pi x = zvm_op_nor(zvm_op_input(0), zvm_op_input(1));
	zvm_op_output(0, zvm_op_nor(x, x));
	return zvm_end_module();
}

static uint32_t emit_not()
{
	zvm_begin_module(1, 1);
	struct zvm_pi i0 = zvm_op_input(0);
	zvm_op_output(0, zvm_op_nor(i0, i0));
	return zvm_end_module();
}

int main(int argc, char** argv)
{
	zvm_init();

	zvm_begin_program();

	module_id_and = emit_and();
	module_id_or = emit_or();
	module_id_not = emit_not();

	zvm_end_program(module_id_not);

	int retvals[100];
	int arguments[100];

	for (int input = 0; input <= 1; input++) {
		arguments[0] = input;
		zvm_run(retvals, arguments);
		printf("NOT(%d) = %d\n", input, retvals[0]);
		zvm_assert((retvals[0] == !input) && "test fail");
	}

	return EXIT_SUCCESS;
}

