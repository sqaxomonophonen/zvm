#include <stdlib.h>
#include <stdio.h>

#include "zvm.h"

uint32_t module_id_and;
uint32_t module_id_or;
uint32_t module_id_not;

uint32_t module_id_memory_bit;

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

static struct zvm_pi mod1(uint32_t module_id, struct zvm_pi x0)
{
	struct zvm_pi pi = zvm_op_instance(module_id);
	zvm_arg(x0);
	pi.i = 0;
	return pi;
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

static struct zvm_pi op_and(struct zvm_pi x0, struct zvm_pi x1)
{
	return mod2(module_id_and, x0, x1);
}

static struct zvm_pi op_not(struct zvm_pi x0)
{
	return mod1(module_id_not, x0);
}

static uint32_t emit_decoder(int n_in)
{
	const int n_out = 1 << n_in;
	zvm_begin_module(n_in, n_out);

	const int MAX_IN = 8;
	zvm_assert(n_in <= MAX_IN);
	struct zvm_pi inputs[MAX_IN];
	for (int j = 0; j < n_in; j++) inputs[j] = zvm_op_input(j);

	for (int i = 0; i < n_out; i++) {
		struct zvm_pi x = {0};
		int m = 1;
		for (int j = 0; j < n_in; j++, m<<=1) {
			struct zvm_pi y = i&m ? inputs[j] : op_not(inputs[j]);
			x = (j == 0) ? (y) : (op_and(x, y));
		}
		zvm_op_output(i, x);
	}
	return zvm_end_module();
}

static uint32_t emit_memory_bit()
{
	zvm_begin_module(2, 1);
	const struct zvm_pi WE = zvm_op_input(0);
	const struct zvm_pi IN = zvm_op_input(1);
	struct zvm_pi dly = zvm_op_unit_delay(ZVM_PI_PLACEHOLDER);
	zvm_op_output(0, dly);
	zvm_assign_arg(dly.p, 0, op_or(op_and(op_not(WE), dly), op_and(WE, IN)));
	return zvm_end_module();
}

static uint32_t emit_memory_byte()
{
	zvm_begin_module(10, 8);
	const struct zvm_pi RE = zvm_op_input(0);
	const struct zvm_pi WE = zvm_op_input(1);
	for (int i = 0; i < 8; i++) {
		struct zvm_pi in = zvm_op_input(2+i);
		struct zvm_pi bit = zvm_pii(zvm_op_instance(module_id_memory_bit), 0);
		zvm_arg(WE);
		zvm_arg(in);
		zvm_op_output(i, op_and(RE, bit));
	}

	return zvm_end_module();
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

	// TEST DECODER
	for (int n = 1; n <= 5; n++) {
		zvm_begin_program();
		emit_functions();
		zvm_end_program(emit_decoder(n));

		int n2 = 1<<n;
		for (int a = 0; a < n2; a++) {
			printf("DEMUX_%d_TO_%d(", n, n2);
			for (int input = 0; input < n; input++) {
				int v = arguments[input] = !!(a & (1 << input));
				if (input > 0) printf(",");
				printf("%d", v);
			}
			zvm_run(retvals, arguments);
			printf(") => [");
			for (int output = 0; output < n2; output++) {
				if (output > 0) printf(",");
				printf("%d", retvals[output]);

				zvm_assert(retvals[output] == (output == a));
			}
			printf("]\n");
		}
	}

	// TEST STATE
	{
		zvm_begin_program();
		emit_functions();
		zvm_end_program(emit_memory_bit());

		int* WE = &arguments[0];
		int* IN = &arguments[1];

		#define T(w,i,x) { *WE=w; *IN=i; zvm_run(retvals, arguments); zvm_assert(retvals[0] == x); }

		T(0,0,0);
		T(1,0,0);
		T(0,0,0);
		T(0,1,0);
		T(1,1,0);
		T(0,0,1);
		T(0,1,1);
		T(0,0,1);
		T(0,1,1);
		T(1,0,1);
		T(1,0,0);
		T(0,0,0);
	}

	// TEST STATE 2
	{
		zvm_begin_program();
		emit_functions();
		module_id_memory_bit = emit_memory_bit();
		zvm_end_program(emit_memory_byte());

		int* RE = &arguments[0];
		int* WE = &arguments[1];
		int* DI = &arguments[2];
		int* DO = &retvals[0];

		for (int i = 0; i < 256; i++) {
			// write value
			*RE = 0;
			*WE = 1;
			for (int j = 0; j < 8; j++) DI[j] = (i>>j)&1;
			zvm_run(retvals, arguments);
			for (int j = 0; j < 8; j++) zvm_assert(DO[j] == 0);

			// read value
			*RE = 1;
			*WE = 0;
			zvm_run(retvals, arguments);
			for (int j = 0; j < 8; j++) zvm_assert(DO[j] == ((i>>j)&1));

			// read value and write zero
			*RE = 1;
			*WE = 1;
			for (int j = 0; j < 8; j++) DI[j] = 0;
			zvm_run(retvals, arguments);
			for (int j = 0; j < 8; j++) zvm_assert(DO[j] == ((i>>j)&1));

			// read zero
			*RE = 1;
			*WE = 0;
			zvm_run(retvals, arguments);
			for (int j = 0; j < 8; j++) zvm_assert(DO[j] == 0);
		}
	}

	printf("\nIT IS OK!\n");

	return EXIT_SUCCESS;
}

