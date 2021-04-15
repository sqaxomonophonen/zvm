#include <stdlib.h>
#include <stdio.h>

#include "zvm.h"


uint32_t module_id_and;
uint32_t module_id_or;
uint32_t module_id_not;
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
	uint32_t x0 = zvm_input(0);
	uint32_t x1 = zvm_input(1);
	uint32_t y0 = zvm_op_nor(x0, x0);
	uint32_t y1 = zvm_op_nor(x1, x1);
	uint32_t and = zvm_op_nor(y0, y1);
	zvm_assign_output(0, and);
	return zvm_end_module();
}

static uint32_t emit_or()
{
	zvm_begin_module(2, 1);
	uint32_t x = zvm_op_nor(zvm_input(0), zvm_input(1));
	uint32_t or = zvm_op_nor(x, x);
	zvm_assign_output(0, or);
	return zvm_end_module();
}

static uint32_t emit_not()
{
	zvm_begin_module(1, 1);
	uint32_t x0 = zvm_input(0);
	uint32_t not = zvm_op_nor(x0, x0);
	zvm_assign_output(0, not);
	return zvm_end_module();
}


static uint32_t mod1(uint32_t module_id, uint32_t x0)
{
	uint32_t id = zvm_op_instance(module_id);
	zvm_arg(x0);
	return id;
}

static uint32_t mod2(uint32_t module_id, uint32_t x0, uint32_t x1)
{
	uint32_t id = zvm_op_instance(module_id);
	zvm_arg(x0);
	zvm_arg(x1);
	return id;
}

static uint32_t op_and(uint32_t x0, uint32_t x1)
{
	return mod2(module_id_and, x0, x1);
}

static uint32_t op_or(uint32_t x0, uint32_t x1)
{
	return mod2(module_id_or, x0, x1);
}

static uint32_t op_not(uint32_t x0)
{
	return mod1(module_id_not, x0);
}

static uint32_t emit_decoder(int n_in)
{
	const int n_out = 1 << n_in;
	zvm_begin_module(n_in, n_out);
	for (int i = 0; i < n_out; i++) {
		uint32_t x = 0;
		int m = 1;
		for (int j = 0; j < n_in; j++, m<<=1) {
			uint32_t y = i&m ? x : op_not(x);
			x = j == 0 ? y : op_and(x, y);

		}
		zvm_assign_output(i, x);
	}
	return zvm_end_module();
}

static uint32_t emit_memory_bit()
{
	zvm_begin_module(2, 1);
	uint32_t WE = zvm_input(0);
	uint32_t IN = zvm_input(1);
	uint32_t dly = zvm_op_unit_delay(ZVM_PLACEHOLDER);
	zvm_assign_output(0, dly);
	zvm_assign_arg(dly, 0, op_or(op_and(op_not(WE), dly), op_and(WE, IN)));
	return zvm_end_module();
}

static uint32_t emit_memory_byte()
{
	zvm_begin_module(10, 8);
	uint32_t RE = zvm_input(0);
	uint32_t WE = zvm_input(1);
	for (int i = 0; i < 8; i++) {
		uint32_t in = zvm_input(2+i);
		uint32_t bit = zvm_op_instance(module_id_memory_bit);
		zvm_arg(WE);
		zvm_arg(in);
		zvm_assign_output(i, op_and(RE, bit));
	}

	return zvm_end_module();
}

static uint32_t emit_ram16(int address_bus_size, uint32_t ram_module_id)
{
	zvm_begin_module(10+address_bus_size, 8);
	uint32_t RE = zvm_input(0);
	uint32_t WE = zvm_input(1);
	const int D = 2;
	const int A = D+8;
	const int n_pass = address_bus_size - 4;
	const int As = A + n_pass;

	uint32_t demux = zvm_op_instance(module_id_decode4to16);
	for (int i = 0; i < 4; i++) zvm_arg(zvm_input(As+i));

	uint32_t memarr[16];

	for (int i = 0; i < 16; i++) {
		uint32_t select = zvm_op_unpack(i, demux);
		uint32_t re = op_and(RE, select);
		uint32_t we = op_and(WE, select);
		memarr[i] = zvm_op_instance(ram_module_id);
		zvm_arg(re);
		zvm_arg(we);
		for (int j = 0; j < 8; j++) zvm_arg(zvm_input(j));
		for (int j = 0; j < n_pass; j++) zvm_arg(A+j);
	}

	for (int i = 0; i < 8; i++) {
		uint32_t x = 0;
		for (int j = 0; j < 16; j++) {
			uint32_t o = zvm_op_unpack(i, memarr[j]);
			x = (j == 0) ? (o) : (op_or(x, o));
		}
		zvm_assign_output(i, x);
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

	//zvm_begin_module(0, 0, 0);
	//uint32_t main_module_id = zvm_end_module();

	zvm_end_program(module_id_ram64k);

	return EXIT_SUCCESS;
}
