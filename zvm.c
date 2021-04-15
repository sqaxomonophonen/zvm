#include <stdlib.h>
#include <assert.h>

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
	(void)zvm_arradd(m.code, mpad(&m));
	zvm_arrpush(ZVM_PRG->modules, m);
	return id;
}

int zvm_end_module()
{
	return zvm_arrlen(ZVM_PRG->modules) - 1;
}
