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

static uint32_t emit_memory_word(int word_size)
{
	zvm_begin_module(2 + word_size, word_size);
	const struct zvm_pi RE = zvm_op_input(0);
	const struct zvm_pi WE = zvm_op_input(1);
	for (int i = 0; i < word_size; i++) {
		struct zvm_pi in = zvm_op_input(2+i);
		struct zvm_pi bit = zvm_pii(zvm_op_instance(module_id_memory_bit), 0);
		zvm_arg(WE);
		zvm_arg(in);
		zvm_op_output(i, op_and(RE, bit));
	}
	return zvm_end_module();
}

int n_decoders;
#define DECODER_MAX 12
uint32_t module_id_decoder[DECODER_MAX];
static uint32_t get_decoder(int n)
{
	zvm_assert(0 < n && (n-1) <= DECODER_MAX);
	while (n_decoders < n) {
		const int address_size = n_decoders+1;
		const int n_in = 1 + address_size;
		const int n_out = 1 << address_size;
		zvm_begin_module(n_in, n_out);
		if (n_decoders == 0) {
			const struct zvm_pi E = zvm_op_input(0);
			const struct zvm_pi A = zvm_op_input(1);
			zvm_op_output(0, op_and(op_not(A), E));
			zvm_op_output(1, op_and(A, E));
		} else {
			const struct zvm_pi E = zvm_op_input(0);
			struct zvm_pi A[DECODER_MAX];
			for (int i = 0; i < address_size; i++) {
				A[i] = zvm_op_input(1+i);
			}

			struct zvm_pi demux_front = zvm_op_instance(module_id_decoder[0]);
			zvm_arg(E);
			zvm_arg(A[address_size-1]);

			struct zvm_pi demux_back[2];
			for (int i = 0; i < 2; i++) {
				demux_back[i] = zvm_op_instance(module_id_decoder[n_decoders-1]);
				zvm_arg(zvm_pii(demux_front, i));
				for (int j = 0; j < (address_size-1); j++) {
					zvm_arg(A[j]);
				}
				const int halfn = 1 << (address_size-1);
				for (int j = 0; j < halfn; j++) {
					zvm_op_output(i*halfn + j, zvm_pii(demux_back[i], j));
				}
			}
		}
		module_id_decoder[n_decoders++] = zvm_end_module();
	}
	return module_id_decoder[n-1];
}

#define MAX_WORD_SIZE 32
#define MAX_ADDRESS_SIZE 24

static uint32_t emit_ram(int word_size, int address_bus_size, int decoder_size)
{
	zvm_assert(address_bus_size >= 0);
	zvm_assert(word_size <= MAX_WORD_SIZE);
	zvm_assert(address_bus_size <= MAX_ADDRESS_SIZE);

	if (address_bus_size == 0) {
		return emit_memory_word(word_size);
	} else {
		if (decoder_size > address_bus_size) {
			decoder_size = address_bus_size;
		}

		uint32_t bb_module_id = emit_ram(word_size, address_bus_size - decoder_size, decoder_size);
		uint32_t demux_module_id = get_decoder(decoder_size);

		const int n_in = 2 + word_size + address_bus_size;
		const int n_out = word_size;
		zvm_begin_module(n_in, n_out);

		int ii = 0;
		const struct zvm_pi RE = zvm_op_input(ii++);
		const struct zvm_pi WE = zvm_op_input(ii++);
		struct zvm_pi D[MAX_WORD_SIZE];
		for (int i = 0; i < word_size; i++) D[i] = zvm_op_input(ii++);
		struct zvm_pi A[MAX_ADDRESS_SIZE];
		for (int i = 0; i < address_bus_size; i++) A[i] = zvm_op_input(ii++);

		struct zvm_pi demux_re = zvm_op_instance(demux_module_id);
		zvm_arg(RE);
		for (int i = 0; i < decoder_size; i++) {
			zvm_arg(A[address_bus_size - decoder_size + i]);
		}

		struct zvm_pi demux_we = zvm_op_instance(demux_module_id);
		zvm_arg(WE);
		for (int i = 0; i < decoder_size; i++) {
			zvm_arg(A[address_bus_size - decoder_size + i]);
		}

		struct zvm_pi O[MAX_WORD_SIZE];

		for (int i = 0; i < (1 << decoder_size); i++) {
			struct zvm_pi bb = zvm_op_instance(bb_module_id);
			zvm_arg(zvm_pii(demux_re, i));
			zvm_arg(zvm_pii(demux_we, i));
			for (int j = 0; j < word_size; j++) zvm_arg(D[j]);
			for (int j = 0; j < (address_bus_size - decoder_size); j++) zvm_arg(A[j]);
			for (int j = 0; j < word_size; j++) {
				struct zvm_pi x = zvm_pii(bb, j);
				O[j] = (i == 0) ? x : op_or(O[j], x);
			}
		}

		for (int j = 0; j < word_size; j++) {
			zvm_op_output(j, O[j]);
		}

		return zvm_end_module();
	}
}

int retvals[100];
int arguments[100];

static void ram_op(int re, int we, int d, int a)
{
	arguments[0] = re;
	arguments[1] = we;
	for (int i = 0; i < 8; i++) arguments[2+i] = !!(d & (1<<i));
	for (int i = 0; i < 16; i++) arguments[10+i] = !!(a & (1<<i));
	zvm_run(retvals, arguments);
}

static void ram_write(int address, int data)
{
	ram_op(0,1,data,address);
}

static int ram_read(int address)
{
	ram_op(1,0,0,address);
	int r = 0;
	for (int i = 0; i < 8; i++) {
		r |= retvals[i] << i;
	}
	return r;
}

static void ramtest(int address_size)
{
	printf("ramtest address size %d\n", address_size);

	zvm_begin_program();

	module_id_and = emit_and();
	module_id_or = emit_or();
	module_id_not = emit_not();
	module_id_memory_bit = emit_memory_bit();

	#define MOD(name) printf("Module[" #name "]=%d\n", module_id_ ## name)
	MOD(and);
	MOD(or);
	MOD(not);
	MOD(memory_bit);
	printf("\n");
	#undef MOD

	const int data_bus_size = 8;
	const int decoder_size = 4;
	uint32_t ram_module_id = emit_ram(data_bus_size, address_size, decoder_size);

	zvm_end_program(ram_module_id);

	printf("\n");

	const int n_addresses = 1 << address_size;

	for (int a = 0; a < n_addresses; a++) {
		ram_write(a, a*a);
	}
	for (int a = 0; a < n_addresses; a++) {
		zvm_assert(ram_read(a) == ((a*a)&0xff));
	}
	for (int a = (n_addresses-1); a >= 0; a--) {
		zvm_assert(ram_read(a) == ((a*a)&0xff));
	}
	for (int a = 0; a < n_addresses; a++) {
		ram_write(a, 0);
	}
	for (int a = 0; a < n_addresses; a++) {
		zvm_assert(ram_read(a) == 0);
	}
}

int main(int argc, char** argv)
{
	zvm_init();
	//ramtest(0);
	//ramtest(4);
	ramtest(8);
	//ramtest(12);
	//ramtest(16);

	printf("YEAH OK\n");

	return EXIT_SUCCESS;
}


