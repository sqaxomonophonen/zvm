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
	memset(ZVM, 0, sizeof(*ZVM));
}

void zvm_begin_program()
{
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

static inline int bs32_test(uint32_t* bs, int i)
{
	return (bs[i>>5] >> (i&31)) & 1;
}

static inline void bs32_set(uint32_t* bs, int i)
{
	bs[i>>5] |= 1 << (i&31);
}

static inline uint32_t bs32_mask(int n)
{
	return (1<<n)-1;
}

static inline int bs32_equal(int n, uint32_t* a, uint32_t* b)
{
	for (; n>32; a++,b++,n-=32) if (*a != *b) return 0;
	if ((*a & bs32_mask(n)) != (*b & bs32_mask(n))) return 0;
	return 1;
}

static inline void bs32_union_inplace(int n, uint32_t* dst, uint32_t* src)
{
	for (; n>32; dst++,src++,n-=32) *dst |= *src;
	*dst |= *src & bs32_mask(n);
}

static inline int bs32_popcnt(int n, uint32_t* bs)
{
	int popcnt = 0;
	for (int i = 0; i < n; i++) if (bs32_test(bs, i)) popcnt++;
	return popcnt;
}

static inline void bs32_fill(int n, uint32_t* bs)
{
	for (; n>32; bs++,n-=32) *bs = ~0;
	*bs |= bs32_mask(n);
}

#if 0
static inline void bs32_intersection_inplace(int n, uint32_t* dst, uint32_t* src)
{
	for (; n>32; dst++,src++,n-=32) *dst &= *src;
	(*dst) &= *src | ~bs32_mask(n);
}
#endif


static inline int bs32_n_words(int n_bits)
{
	return (n_bits + 31) >> 5;
}

static inline int n_io_bitset_words(struct zvm_module* mod)
{
	return bs32_n_words(mod->n_inputs);
}

static uint32_t* get_io_bitset(struct zvm_module* mod, int output_index)
{
	assert(0 <= output_index && output_index < mod->n_outputs);
	return &mod->code[mod->io_bitsets_index + output_index * n_io_bitset_words(mod)];
}

static void trace_dependencies_rec(uint32_t* io_bitset, uint32_t p, int unpack_index)
{
	if (p == ZVM_ZERO) return;
	assert(p < ZVM_SPECIAL);

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
				trace_dependencies_rec(io_bitset, mod->code[zvm__arg_index(p, i)], -1);
			}
		}
	} else if (op == ZVM_OP(UNPACK)) {
		zvm_assert(unpack_index == -1 && "unexpected unpack");
		int index = code >> ZVM_OP_BITS;
		// XXX no recursion necessary here... and eliminating it would
		// remove the need for unpack_index?
		trace_dependencies_rec(io_bitset, mod->code[zvm__arg_index(p, 0)], index);
	} else if (op == ZVM_OP(UNIT_DELAY)) {
		zvm_assert(unpack_index == -1 && "unexpected unpack");
		// unit delays have no dependencies
	} else {
		zvm_assert(unpack_index == -1 && "unexpected unpack");
		int n_args = zvm__op_n_args(code);
		for (int i = 0; i < n_args; i++) {
			trace_dependencies_rec(io_bitset, mod->code[zvm__arg_index(p, i)], -1);
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
		trace_dependencies_rec(io_bitset, p, -1);
		#ifdef VERBOSE_DEBUG
		printf("output %d depends on inputs:", i);
		for (int j = 0; j < mod->n_inputs; j++) if (bs32_test(io_bitset, j)) printf(" %d", j);
		printf("\n");
		#endif
	}
	// TODO maybe also trace state dependencies? instead of tracing the
	// dependencies of an output, I'd trace the dependencies for unit delay
	// inputs, and instance inputs that are part of the instance's state
	// dependencies
	#ifdef VERBOSE_DEBUG
	printf("bits: %d\n", mod->n_bits);
	#endif

	return zvm_arrlen(ZVM_PRG->modules) - 1;
}

/*
TODO ops:
  OP0 arithmetic: CONST_ZERO, CONST_ONE
  OP1 arithmetic: NOT, MOVE...
  OP2 arithmetic: NOR, AND, OR, ...
  SPECIAL: CALL, RETURN,...
*/

static void emit_mov(uint32_t dst, uint32_t src)
{
	// TODO
}

static void emit_zero(uint32_t dst)
{
	// TODO
}

static void emit_read(uint32_t dst, uint32_t state_src)
{
	// TODO
}

static void emit_nor(uint32_t dst, uint32_t a, uint32_t b)
{
	// TODO
}

static uint32_t alloc_reg(struct zvm_function* fn)
{
	return -1; // XXX
}

static void emit_code(struct zvm_function* fn, uint32_t dst_reg, uint32_t p)
{
	struct zvm_module* mod = &ZVM_PRG->modules[fn->module_id];
	if (p < mod->n_inputs) {
		zvm_assert((fn->n_args <= dst_reg && dst_reg < fn->n_args+fn->n_retvals) && "only arg->retval mov expected here");
		uint32_t src_reg = fn->input_arg_map[p];
		zvm_assert(src_reg != -1);
		emit_mov(dst_reg, src_reg);
		return;
	}

	if (p == ZVM_ZERO) {
		emit_zero(dst_reg);
		return;
	}

	zvm_assert(p < ZVM_SPECIAL);

	uint32_t code = mod->code[p];

	uint32_t op = code & ZVM_OP_MASK;
	if (op == ZVM_OP(INSTANCE)) {
		// TODO assert one-output module? (otherwise we'd be in UNPACK)
	} else if (op == ZVM_OP(UNPACK)) {
		int index = code >> ZVM_OP_BITS;
		uint32_t code2 = mod->code[zvm__arg_index(p, 0)];
		uint32_t op2 = code2 & ZVM_OP_MASK;
		zvm_assert(op2 == ZVM_OP(INSTANCE) && "non-instance unpack");
		int mod2_id = code2 >> ZVM_OP_BITS;
		struct zvm_module* mod2 = &ZVM_PRG->modules[mod2_id];
		zvm_assert(index < mod2->n_outputs);

		const int n_functions = zvm_arrlen(ZVM_PRG->functions);
		for (int i = 0; i < n_functions; i++) {
			struct zvm_function* fn = &ZVM_PRG->functions[i];
			if (fn->module_id == mod2_id && bs32_test(fn->module_outputs_bs32, index)) {
				// found a matching function... now what? XXX :)
				break;
			}
		}

		// XXX several trace paths could end here... meaning the CALL
		// must already be executed, otherwise we cannot output code...
		// so we'd have to output simply a register... or a move or
		// something?


	} else if (op == ZVM_OP(UNIT_DELAY)) {
		int state_index = 0; // XXX how do I know this? requires enumeration of unit delays...
		emit_read(dst_reg, state_index);
	} else {

		int n_args = zvm__op_n_args(code);
		uint32_t args[2];
		zvm_assert(n_args <= ZVM_ARRAY_LENGTH(args));
		for (int i = 0; i < n_args; i++) {
			args[i] = alloc_reg(fn);
		}
		for (int i = 0; i < n_args; i++) {
			emit_code(fn, args[i], mod->code[zvm__arg_index(p, i)]);
		}

		if (op == ZVM_OP(NOR)) {
			zvm_assert(n_args == 2);
			emit_nor(dst_reg, args[0], args[1]);
		} else {
			zvm_assert(!"unhandled op");
		}
	}
}

static void emit_function(uint32_t module_id, uint32_t* output_bs32)
{
	struct zvm_module* mod = &ZVM_PRG->modules[module_id];

	#ifdef DEBUG
	{
		const int n_functions = zvm_arrlen(ZVM_PRG->functions);
		for (int i = 0; i < n_functions; i++) {
			struct zvm_function* fn = &ZVM_PRG->functions[i];
			zvm_assert((fn->module_id != module_id || !bs32_equal(mod->n_outputs, fn->module_outputs_bs32, output_bs32)) && "duplicate function");
		}
	}
	#endif

	uint32_t* input_bs32 = zvm_arradd(ZVM_PRG->scratch, n_io_bitset_words(mod));
	memset(input_bs32, 0, n_io_bitset_words(mod) * sizeof(*input_bs32));
	for (int i = 0; i < mod->n_outputs; i++) {
		bs32_union_inplace(mod->n_inputs, input_bs32, get_io_bitset(mod, i));
	}

	uint32_t* input_arg_map = zvm_arradd(ZVM_PRG->scratch, mod->n_inputs);
	{
		int arg_index = 0;
		for (int i = 0; i < mod->n_inputs; i++) {
			if (bs32_test(input_bs32, i)) {
				input_arg_map[i] = arg_index++;
			} else {
				input_arg_map[i] = -1;
			}
		}
	}

	struct zvm_function* fn = zvm_arradd(ZVM_PRG->functions, 1);
	memset(fn, 0, sizeof *fn);
	fn->module_id = module_id;
	fn->start = zvm_arrlen(ZVM_PRG->bytecode);
	fn->n_args = bs32_popcnt(mod->n_inputs, input_bs32);
	fn->n_retvals = bs32_popcnt(mod->n_outputs, output_bs32);
	fn->module_inputs_bs32 = input_bs32;
	fn->module_outputs_bs32 = output_bs32;
	fn->input_arg_map = input_arg_map;

	uint32_t dst_reg = fn->n_args;
	for (int i = 0; i < mod->n_outputs; i++) {
		if (!bs32_test(output_bs32, i)) continue;
		emit_code(fn, dst_reg, mod->code[i]);
		dst_reg++;
	}
	// XXX FIXME what about unit delay inputs?
}

void zvm_end_program(uint32_t main_module_id)
{
	ZVM_PRG->main_module_id = main_module_id;

	/*
	XXX proper approach: I need a forward traversal structure:
	 - do drain->source recursion; insert each seen
	   [code_index,output_index] pair in an array (i.e. the output_index
	   for mod->code[code_index])
	 - when done, qsort the list (ORDER BY code_index,output_index)
	 - then compact the list (no duplicates)
	now I can binary search that list.
	 - I could then do drain->source recursion again, and increment a
	   refcount for each node. when done it tells me how many receivers
	   each node has.. and then finally I can insert the receivers
	I can then begin using this structure to push forward from "known
	values" and actually emit code.. and I suppose I can attach any data or
	metadata or state to these nodes...
	*/

	const int n_outputs = ZVM_PRG->modules[main_module_id].n_outputs;
	uint32_t* output_bs32 = zvm_arradd(ZVM_PRG->scratch, bs32_n_words(n_outputs));
	bs32_fill(n_outputs, output_bs32);
	emit_function(main_module_id, output_bs32);
}
