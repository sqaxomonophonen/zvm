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

static uint32_t buftop()
{
	return zvm_arrlen(ZVM_PRG->buf);
}

void zvm_init()
{
	zvm_assert(ZVM_OP_N <= ZVM_OP_MASK);
	memset(ZVM, 0, sizeof(*ZVM));
}

void zvm_begin_program()
{
}

int zvm_begin_module(int n_inputs, int n_outputs)
{
	int id = zvm_arrlen(ZVM_PRG->modules);
	struct zvm_module m = {0};
	m.n_inputs = n_inputs;
	m.n_outputs = n_outputs;
	m.outputs_p = buftop();
	(void)zvm_arradd(ZVM_PRG->buf, n_outputs);
	m.code_begin_p = buftop();
	zvm_arrpush(ZVM_PRG->modules, m);
	return id;
}

static inline int bs32_n_words(int n_bits)
{
	return (n_bits + 31) >> 5;
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

static inline void bs32_clear(int n, uint32_t* bs)
{
	memset(bs, 0, sizeof(*bs) * bs32_n_words(n));
}

static inline void bs32_fill(int n, uint32_t* bs)
{
	memset(bs, ~0, sizeof(*bs) * bs32_n_words(n));
}

static inline void bs32_intersection_inplace(int n, uint32_t* dst, uint32_t* src)
{
	for (; n>32; dst++,src++,n-=32) *dst &= *src;
	(*dst) &= *src | ~bs32_mask(n);
}


static void bs32_print(int n, uint32_t* bs)
{
	printf("[");
	int first = 1;
	for (int i = 0; i < n; i++) {
		if (bs32_test(bs, i)) {
			printf("%s%d", (first ? "" : " "), i);
			first = 0;
		}
	}
	printf("]");
}

static inline int mod_n_input_bs32_words(struct zvm_module* mod)
{
	return bs32_n_words(mod->n_inputs);
}

static uint32_t* get_input_bs32(struct zvm_module* mod, int index)
{
	return &ZVM_PRG->buf[mod->input_bs32s_p + index * mod_n_input_bs32_words(mod)];
}

static uint32_t* get_state_input_dep_bs32(struct zvm_module* mod)
{
	return get_input_bs32(mod, 0);
}

static uint32_t* get_output_input_dep_bs32(struct zvm_module* mod, int output_index)
{
	assert(0 <= output_index && output_index < mod->n_outputs);
	return get_input_bs32(mod, 1+output_index);
}

static int get_op_length(uint32_t p)
{
	uint32_t code = ZVM_PRG->buf[p];
	int op = code & ZVM_OP_MASK;
	if (op == ZVM_OP(INSTANCE)) {
		int module_id = code >> ZVM_OP_BITS;
		zvm_assert(zvm__is_valid_module_id(module_id));
		struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];
		return 1 + mod2->n_inputs;
	} else if (op == ZVM_OP(UNPACK)) {
		return 2;
	} else if (op == ZVM_OP(UNIT_DELAY)) {
		return 2;
	} else if (op == ZVM_OP(NOR)) {
		// XXX should probably group this into 2in1out, 1in1out, etc...
		return 3;
	} else {
		zvm_assert(!"unhandled op");
		return 0;
	}
}

static int get_op_n_outputs(uint32_t p)
{
	uint32_t code = ZVM_PRG->buf[p];
	int op = code & ZVM_OP_MASK;
	if (op == ZVM_OP(INSTANCE)) {
		int module_id = code >> ZVM_OP_BITS;
		zvm_assert(zvm__is_valid_module_id(module_id));
		struct zvm_module* mod = &ZVM_PRG->modules[module_id];
		return mod->n_outputs;
	} else if (op == ZVM_OP(UNPACK)) {
		return 1;
	} else if (op == ZVM_OP(UNIT_DELAY)) {
		return 1;
	} else if (op == ZVM_OP(NOR)) {
		return 1;
	} else {
		zvm_assert(!"unhandled op");
		return 0;
	}
}

static int nodecmp(const void* va, const void* vb)
{
	const uint32_t* a = va;
	const uint32_t* b = vb;
	if (a[0] < b[0])  return -1;
	if (a[0] > b[0])  return  1;
	if (a[1] < b[1])  return -1;
	if (a[1] > b[1])  return  1;
	return 0;
}

static void build_node_table(struct zvm_module* mod)
{
	uint32_t* nodes = NULL;
	uint32_t* np = NULL;
	for (int pass = 0; pass < 2; pass++) {
		int n_nodes_total = 0;

		uint32_t p = mod->code_begin_p;
		const uint32_t p_end = mod->code_end_p;
		while (p < p_end) {
			int n_nodes = get_op_n_outputs(p);

			if (pass == 1) {
				for (int i = 0; i < n_nodes; i++) {
					//printf("->%d:%d\n",p,i);
					*(np++) = p;
					*(np++) = i;
				}
			}

			n_nodes_total += n_nodes;
			p += get_op_length(p);
		}
		zvm_assert(p == p_end);

		if (pass == 0) {
			mod->n_nodes = n_nodes_total;
			mod->nodes_p = buftop();
			np = nodes = zvm_arradd(ZVM_PRG->buf, 2*mod->n_nodes);
		} else if (pass == 1) {
			#if 0
			// qsort not necessary; nodes are inserted in ascending
			// order
			qsort(nodes, mod->n_nodes, 2*sizeof(*nodes), nodecmp);
			#endif
			#if 0
			printf("nodes (n=%d): ", mod->n_nodes);
			for (int i = 0; i < mod->n_nodes; i++) printf(" %d:%d", nodes[i*2], nodes[i*2+1]);
			printf("\n");
			#endif
		}
	}
}

#if 0
static inline uint32_t* get_nodedata0(struct zvm_module* mod)
{
	return &ZVM_PRG->buf[mod->nodedata0_p];
}

static void clear_nodedata0(struct zvm_module* mod)
{
	memset(get_nodedata0(mod), 0, sizeof(ZVM_PRG->buf) * mod->n_nodes);
}
#endif

static int get_node_index(struct zvm_module* mod, uint32_t p, uint32_t i)
{
	uint32_t* nodes = &ZVM_PRG->buf[mod->nodes_p];
	const uint32_t k[] = {p,i};
	int left = 0;
	int right = mod->n_nodes - 1;
	while (left <= right) {
		int mid = (left+right) >> 1;
		int cmp = nodecmp(&nodes[mid*2], k);
		if (cmp < 0) {
			left = mid + 1;
		} else if (cmp > 0) {
			right = mid - 1;
		} else {
			return mid;
		}
	}

	//printf("NOT FOUND: %d:%d\n", p, i);
	zvm_assert(!"node not found");
}

uint32_t* get_node_bs32(struct zvm_module* mod)
{
	return &ZVM_PRG->buf[mod->node_bs32_p];
}

static void clear_node_visit_set(struct zvm_module* mod)
{
	bs32_clear(mod->n_nodes, get_node_bs32(mod));
}

static int visit_node(struct zvm_module* mod, uint32_t p, uint32_t output_index)
{
	int node_index = get_node_index(mod, p, output_index);
	uint32_t* node_bs32 = get_node_bs32(mod);
	if (bs32_test(node_bs32, node_index)) {
		return 0;
	} else {
		bs32_set(node_bs32, node_index);
		return 1;
	}
}

static void trace_inputs_rec(struct zvm_module* mod, uint32_t* input_bs32, uint32_t p)
{
	int unpack_index = -1;

	for (;;) {
		if (ZVM_IS_SPECIALX(p, ZVM_X_CONST)) {
			zvm_assert(unpack_index == -1);
			return;
		}

		if (ZVM_IS_SPECIALX(p, ZVM_X_INPUT)) {
			zvm_assert(unpack_index == -1);
			bs32_set(input_bs32, ZVM_GET_SPECIALY(p));
			return;
		}

		assert(!ZVM_IS_SPECIAL(p));

		zvm_assert((mod->code_begin_p <= p && p < mod->code_end_p) && "p out of range");

		int output_index = unpack_index >= 0 ? unpack_index : 0;

		if (!visit_node(mod, p, output_index)) {
			return;
		}

		uint32_t code = ZVM_PRG->buf[p];

		int op = code & ZVM_OP_MASK;
		if (op == ZVM_OP(INSTANCE)) {
			int module_id = code >> ZVM_OP_BITS;
			zvm_assert(zvm__is_valid_module_id(module_id));
			struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];
			if (mod2->n_outputs == 1 && unpack_index == -1) unpack_index = 0;
			zvm_assert(0 <= unpack_index && unpack_index < mod2->n_outputs && "expected valid unpack");
			uint32_t* bs32 = get_output_input_dep_bs32(mod2, unpack_index);
			for (int i = 0; i < mod2->n_inputs; i++) {
				if (bs32_test(bs32, i)) {
					trace_inputs_rec(mod, input_bs32, ZVM_PRG->buf[zvm__arg_index(p, i)]);
				}
			}
			return;
		}

		zvm_assert(unpack_index == -1 && "unexpected unpack");

		if (op == ZVM_OP(UNPACK)) {
			unpack_index = code >> ZVM_OP_BITS;
			p = ZVM_PRG->buf[zvm__arg_index(p, 0)];
			continue;
		} else if (op == ZVM_OP(UNIT_DELAY)) {
			// unit delays have no dependencies
			return;
		} else {
			int n_args = zvm__op_n_args(code);
			for (int i = 0; i < n_args; i++) {
				trace_inputs_rec(mod, input_bs32, ZVM_PRG->buf[zvm__arg_index(p, i)]);
			}
			return;
		}
	}
}

static void trace_inputs(struct zvm_module* mod, uint32_t* input_bs32, uint32_t p)
{
	clear_node_visit_set(mod);
	trace_inputs_rec(mod, input_bs32, p);
}

static void trace_state_deps(uint32_t* input_bs32, struct zvm_module* mod)
{
	clear_node_visit_set(mod);

	uint32_t p = mod->code_begin_p;
	const uint32_t p_end = mod->code_end_p;
	while (p < p_end) {
		uint32_t code = ZVM_PRG->buf[p];
		int op = code & ZVM_OP_MASK;
		if (op == ZVM_OP(UNIT_DELAY)) {
			trace_inputs_rec(mod, input_bs32, ZVM_PRG->buf[zvm__arg_index(p,0)]);
		} else if (op == ZVM_OP(INSTANCE)) {
			int module_id = code >> ZVM_OP_BITS;
			zvm_assert(zvm__is_valid_module_id(module_id));
			struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];
			uint32_t* ibs = get_state_input_dep_bs32(mod2);
			int n_inputs = mod2->n_inputs;
			for (int i = 0; i < n_inputs; i++) {
				if (bs32_test(ibs, i)) {
					trace_inputs_rec(mod, input_bs32, ZVM_PRG->buf[zvm__arg_index(p,i)]);
				}
			}
		}
		p += get_op_length(p);
	}
	zvm_assert(p == p_end);
}

int zvm_end_module()
{
	struct zvm_module* mod = ZVM_MOD;

	mod->code_end_p = buftop();

	build_node_table(mod);

	mod->node_bs32_p = buftop();
	(void)zvm_arradd(ZVM_PRG->buf, bs32_n_words(mod->n_nodes));

	//mod->nodedata0_p = buftop();
	//(void)zvm_arradd(ZVM_PRG->buf, mod->n_nodes);

	// initialize input bitsets
	{
		const int n_words = mod_n_input_bs32_words(mod);
		mod->input_bs32s_p = buftop();
		const int n_input_bs32s = 1 + mod->n_inputs;
		uint32_t* input_bs32s = zvm_arradd(ZVM_PRG->buf, n_input_bs32s * n_words);
		memset(input_bs32s, 0, sizeof(*input_bs32s) * n_input_bs32s * n_words);
	}

	// trace state input-dependencies
	uint32_t* state_input_dep_bs32   = get_state_input_dep_bs32(mod);
	trace_state_deps(state_input_dep_bs32, mod);
	#ifdef VERBOSE_DEBUG
	printf("state: "); bs32_print(mod->n_inputs, state_input_dep_bs32); printf("\n");
	#endif

	// trace output input-dependencies
	for (int i = 0; i < mod->n_outputs; i++) {
		uint32_t* output_input_dep_bs32 = get_output_input_dep_bs32(mod, i);
		trace_inputs(mod, output_input_dep_bs32, ZVM_PRG->buf[mod->outputs_p + i]);
		#ifdef VERBOSE_DEBUG
		printf("o[%d]: ", i); bs32_print(mod->n_inputs, output_input_dep_bs32); printf("\n");
		#endif
	}

	return zvm_arrlen(ZVM_PRG->modules) - 1;
}

static void emit_function(uint32_t module_id)
{
}

static void emit_main_function(uint32_t main_module_id)
{
	emit_function(main_module_id);
}

void zvm_end_program(uint32_t main_module_id)
{
	ZVM_PRG->main_module_id = main_module_id;
	emit_main_function(main_module_id);
}
