#include <stdlib.h>
#include <stdio.h>

#include "zvm.h"

uint32_t module_id_and;
uint32_t module_id_or;
uint32_t module_id_not;

uint32_t module_id_decode4to16;

uint32_t module_id_memory_bit;
uint32_t module_id_memory_byte;
uint32_t module_id_ram16;
uint32_t module_id_ram256;
uint32_t module_id_ram4k;
uint32_t module_id_ram64k;

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

static struct zvm_pi op_and(struct zvm_pi x0, struct zvm_pi x1)
{
	return mod2(module_id_and, x0, x1);
}

static struct zvm_pi op_or(struct zvm_pi x0, struct zvm_pi x1)
{
	return mod2(module_id_or, x0, x1);
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
		struct zvm_pi bit = zvm_op_instance(module_id_memory_bit);
		bit.i = 0;
		zvm_arg(WE);
		zvm_arg(in);
		zvm_op_output(i, op_and(RE, bit));
	}

	return zvm_end_module();
}

static uint32_t emit_ram16(int address_bus_size, uint32_t ram_module_id)
{
	const int MAX_ADDRESS_BUS_SIZE = 16;
	const int N_STD_INPUTS = 10;
	const int MAX_INPUTS = N_STD_INPUTS + MAX_ADDRESS_BUS_SIZE;
	zvm_assert(address_bus_size <= MAX_ADDRESS_BUS_SIZE);

	const int n_inputs = N_STD_INPUTS + address_bus_size;

	zvm_begin_module(n_inputs, 8);

	struct zvm_pi inputs[MAX_INPUTS];
	for (int i = 0; i < n_inputs; i++) inputs[i] = zvm_op_input(i);

	const struct zvm_pi RE = inputs[0];
	const struct zvm_pi WE = inputs[1];
	const struct zvm_pi* D = &inputs[2];
	const struct zvm_pi* A = &inputs[2+8];
	const int n_pass = address_bus_size - 4;
	const struct zvm_pi* As = &inputs[2+8+n_pass];

	struct zvm_pi demux = zvm_op_instance(module_id_decode4to16);
	for (int i = 0; i < 4; i++) zvm_arg(As[i]);

	struct zvm_pi memarr[16];

	for (int i = 0; i < 16; i++) {
		struct zvm_pi select = zvm_pii(demux, i);
		struct zvm_pi re = op_and(RE, select);
		struct zvm_pi we = op_and(WE, select);
		memarr[i] = zvm_op_instance(ram_module_id);
		zvm_arg(re);
		zvm_arg(we);
		for (int j = 0; j < 8; j++) zvm_arg(D[j]);
		for (int j = 0; j < n_pass; j++) zvm_arg(A[j]);
	}

	for (int i = 0; i < 8; i++) {
		struct zvm_pi x = {0};
		for (int j = 0; j < 16; j++) {
			struct zvm_pi o = zvm_pii(memarr[j], i);
			x = (j == 0) ? (o) : (op_or(x, o));
		}
		zvm_op_output(i, x);
	}

	return zvm_end_module();
}

int main(int argc, char** argv)
{
	zvm_init();

	zvm_begin_program();

	module_id_and = emit_and();
	module_id_or = emit_or();
	module_id_not = emit_not();
	module_id_decode4to16 = emit_decoder(4);
	module_id_memory_bit = emit_memory_bit();
	module_id_memory_byte = emit_memory_byte();
	module_id_ram16 = emit_ram16(4, module_id_memory_byte);
	module_id_ram256 = emit_ram16(8, module_id_ram16);
	module_id_ram4k = emit_ram16(12, module_id_ram256);
	module_id_ram64k = emit_ram16(16, module_id_ram4k);

	#define MOD(name) printf("Module[" #name "]=%d\n", module_id_ ## name)
	MOD(and);
	MOD(or);
	MOD(not);
	MOD(decode4to16);
	MOD(memory_bit);
	MOD(memory_byte);
	MOD(ram16);
	MOD(ram256);
	MOD(ram4k);
	MOD(ram64k);
	printf("\n");
	#undef MOD

	zvm_end_program(module_id_ram64k);

	zvm_run(NULL, NULL);

	return EXIT_SUCCESS;
}

