#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "zvm.h"

struct zvm zvmg;

// stolen from nothings/stb/stretchy_buffer.h
void* zvm__grow_impl(void* xs, int increment, int item_sz)
{
	int double_cap = xs ? 2*zvm__cap(xs) : 0;
	int required = zvm_arrlen(xs) + increment;
	int new_cap = double_cap > required ? double_cap : required;
	int *p = (int *) realloc(xs ? zvm__magic(xs) : NULL, item_sz * new_cap + 2*sizeof(int));
	if (p) {
		if (!xs) p[1] = 0;
		p[0] = new_cap;
		return p+2;
	} else {
		assert(!"realloc() failed");
	}
}

void zvm_init()
{
	zvm_assert(ZVM_OP_N <= ZVM_OP_MASK);
}

void zvm_begin_program()
{
}

void zvm_end_program(uint32_t main_module_id)
{
	zvmg.prg.main_module_id = main_module_id;
}

static inline struct zvm_program* prg()
{
	return &zvmg.prg;
}

int mpad(struct zvm_module* m)
{
	int pad0 = m->n_inputs;
	int pad1 = m->n_outputs;
	return pad0 > pad1 ? pad0 : pad1;
}

int zvm_begin_module(int n_inputs, int n_outputs)
{
	int id = zvm_arrlen(ZVM_PRG->modules);
	struct zvm_module m = {0};
	m.n_inputs = n_inputs;
	m.n_outputs = n_outputs;
	int pad = mpad(&m);
	uint32_t* xs = zvm_arradd(m.code, pad);
	for (int i = 0; i < pad; i++) xs[i] = ZVM_PLACEHOLDER;
	zvm_arrpush(ZVM_PRG->modules, m);
	return id;
}

static inline int n_io_bitset_words(struct zvm_module* mod)
{
	return (mod->n_inputs + 31) >> 5;
}

static uint32_t* get_io_bitset(struct zvm_module* mod, int output_index)
{
	assert(0 <= output_index && output_index < mod->n_outputs);
	return &mod->code[mod->io_bitsets_index + output_index * n_io_bitset_words(mod)];
}

static inline int bs32_test(uint32_t* bs, int i)
{
	return (bs[i>>5] >> i) & 1;
}

static inline void bs32_set(uint32_t* bs, int i)
{
	bs[i>>5] |= 1 << (i&31);
}

static void trace_dependencies(uint32_t* io_bitset, uint32_t p, int unpack_index)
{
	struct zvm_module* mod = ZVM_MOD;
	if (p < mod->n_inputs) {
		bs32_set(io_bitset, p);
		return;
	}

	uint32_t code = mod->code[p];

	int op = code & ZVM_OP_MASK;
	if (op == ZVM_OP(INSTANCE)) {
		int module_id = code >> ZVM_OP_BITS;
		zvm_assert(zvm__is_valid_module_id(module_id));
		struct zvm_module* m2 = &ZVM_PRG->modules[module_id];
		if (m2->n_outputs == 1 && unpack_index == -1) unpack_index = 0;
		zvm_assert(0 <= unpack_index && unpack_index < m2->n_outputs && "expected valid unpack");
		uint32_t* bs = get_io_bitset(m2, unpack_index);
		for (int i = 0; i < m2->n_inputs; i++) {
			if (bs32_test(bs, i)) {
				trace_dependencies(io_bitset, mod->code[zvm__arg_index(p, i)], -1);
			}
		}
	} else if (op == ZVM_OP(UNPACK)) {
		zvm_assert(unpack_index == -1 && "unexpected unpack");
		int index = code >> ZVM_OP_BITS;
		trace_dependencies(io_bitset, mod->code[zvm__arg_index(p, 0)], index);
	} else if (op == ZVM_OP(UNIT_DELAY)) {
		zvm_assert(unpack_index == -1 && "unexpected unpack");
		// unit delays have no dependencies
	} else {
		zvm_assert(unpack_index == -1 && "unexpected unpack");
		int n_args = zvm__op_n_args(code);
		for (int i = 0; i < n_args; i++) {
			trace_dependencies(io_bitset, mod->code[zvm__arg_index(p, i)], -1);
		}
	}
}

int zvm_end_module()
{
	struct zvm_module* mod = ZVM_MOD;

	const int n_words = n_io_bitset_words(mod);
	mod->io_bitsets_index = zvm_arrlen(mod->code);
	for (int i = 0; i < mod->n_outputs; i++) {
		uint32_t p = mod->code[i];
		zvm_assert(p != ZVM_PLACEHOLDER && "unassigned output");
		uint32_t* io_bitset = zvm_arradd(mod->code, n_words);
		memset(io_bitset, 0, n_words*sizeof(*io_bitset));
		trace_dependencies(io_bitset, p, -1);
		#if 0
		printf("output %d depends on inputs:", i);
		for (int j = 0; j < mod->n_inputs; j++) if (bs32_test(io_bitset, j)) printf(" %d", j);
		printf("\n");
		#endif
	}

	return zvm_arrlen(ZVM_PRG->modules) - 1;
}
