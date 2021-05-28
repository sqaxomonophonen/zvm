#include <stdlib.h>
#include <stdio.h>

#include "zvm.h"

static uint32_t emit_and()
{
	zvm_begin_module(2, 1);
	struct zvm_pi i0 = zvm_op_input(0);
	struct zvm_pi i1 = zvm_op_input(1);
	zvm_op_output(0, zvm_op_nor(zvm_op_nor(i0, i0), zvm_op_nor(i1, i1)));
	return zvm_end_module();
}

int main(int argc, char** argv)
{
	zvm_init();
	zvm_begin_program();
	zvm_end_program(emit_and());

	zvm_run(NULL, NULL);

	return EXIT_SUCCESS;
}

