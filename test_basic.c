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

static void emit_functions()
{
	module_id_and = emit_and();
	module_id_or = emit_or();
	module_id_not = emit_not();
}

static struct zvm_pi mod2(uint32_t module_id, struct zvm_pi x0, struct zvm_pi x1)
{
	struct zvm_pi pi = zvm_op_instance(module_id);
	zvm_arg(x0);
	zvm_arg(x1);
	pi.i = 0;
	return pi;
}

static struct zvm_pi op_or(struct zvm_pi x0, struct zvm_pi x1)
{
	return mod2(module_id_or, x0, x1);
}

int retvals[100];
int arguments[100];

int main(int argc, char** argv)
{
	zvm_init();


	// TEST NOT
	{
		zvm_begin_program();
		emit_functions();
		zvm_end_program(module_id_not);

		for (int input = 0; input <= 1; input++) {
			arguments[0] = input;
			zvm_run(retvals, arguments);
			printf("NOT(%d) = %d\n", input, retvals[0]);
			zvm_assert((retvals[0] == !input) && "test fail");
		}
	}

	// TEST AND
	{
		zvm_begin_program();
		emit_functions();
		zvm_end_program(module_id_and);

		for (int input = 0; input < 4; input++) {
			int x = arguments[0] = input & 1;
			int y = arguments[1] = input >> 1;
			zvm_run(retvals, arguments);
			printf("AND(%d,%d) = %d\n", x, y, retvals[0]);
			zvm_assert((retvals[0] == (x&&y)) && "test fail");
		}
	}

	// TEST OR
	{
		zvm_begin_program();
		emit_functions();
		zvm_end_program(module_id_or);

		for (int input = 0; input < 4; input++) {
			int x = arguments[0] = input & 1;
			int y = arguments[1] = (input >> 1) & 1;
			zvm_run(retvals, arguments);
			printf("OR(%d,%d) = %d\n", x, y, retvals[0]);
			zvm_assert((retvals[0] == (x||y)) && "test fail");
		}
	}

	// TEST COMBINATION
	{
		zvm_begin_program();
		emit_functions();

		zvm_begin_module(3, 2);

		struct zvm_pi i0 = zvm_op_input(0);
		struct zvm_pi i1 = zvm_op_input(1);
		struct zvm_pi i2 = zvm_op_input(2);

		zvm_op_output(0, op_or(i0,i1));
		zvm_op_output(1, op_or(i1,i2));

		zvm_end_program(zvm_end_module());

		for (int input = 0; input < 8; input++) {
			int x = arguments[0] = input & 1;
			int y = arguments[1] = (input >> 1) & 1;
			int z = arguments[2] = (input >> 2) & 1;
			zvm_run(retvals, arguments);
			printf("COMB(%d,%d,%d) => [%d,%d]\n", x, y, z, retvals[0], retvals[1]);
			zvm_assert((retvals[0] == (x||y)) && "test fail");
			zvm_assert((retvals[1] == (y||z)) && "test fail");
		}
	}

	printf("\nIT IS OK!\n");

	return EXIT_SUCCESS;
}

