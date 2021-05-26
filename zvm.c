#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "zvm.h"

uint32_t* zvm__buf;

struct module {
	int n_inputs;
	int n_outputs;

	// TODO I/O type signature? they're all bits to begin with, until
	// they're not, e.g. for platform semi-independent optimizations like
	// "integer SIMD", or floating point stuff

	uint32_t code_begin_p;
	uint32_t code_end_p;

	uint32_t input_bs32i;

	uint32_t outputs_i;

	int n_bits;

	int n_node_outputs;
	uint32_t node_outputs_i;
	uint32_t node_output_map_i;
	uint32_t node_output_bs32i;

	uint32_t unit_delay_index_map_i;
	uint32_t unit_delay_index_map_n;
};

struct unit_delay_index {
	uint32_t p;
	uint32_t index;
};

struct node_output {
	uint32_t p;
	uint32_t index;
};

struct output {
	uint32_t p;
};

struct substance_key {
	uint32_t module_id;
	uint32_t outcome_request_bs32i;
};

struct substance_keyval {
	struct substance_key key;
	uint32_t substance_id;
};

struct substance {
	struct substance_key key;
	uint32_t n_steps;
	uint32_t steps_i;
	int tag;
	int refcount;
};

struct drout {
	uint32_t p;
	uint32_t index;
	uint32_t counter;
	uint32_t decr_list_n;
	uint32_t decr_list_i;
	uint32_t usr;
};

struct step {
	uint32_t p;
	uint32_t substance_id;
};

struct function {
	uint32_t substance_id;
	uint32_t bytecode_i;
	uint32_t bytecode_n;
};

struct globals {
	struct module* modules;
	struct node_output* node_outputs;
	uint32_t* node_output_maps;
	struct substance_keyval* substance_keyvals;
	struct substance* substances;
	struct function* functions;
	struct output* outputs;
	struct step* steps;
	uint32_t* bs32s;
	uint32_t* bytecode;
	struct unit_delay_index* unit_delay_index_maps;

	struct drout* tmp_drains;
	struct drout* tmp_outcomes;
	uint32_t* tmp_decr_lists;
	uint32_t* tmp_queue;

	uint32_t main_module_id;
	uint32_t main_substance_id;
} g;

static inline int is_valid_module_id(int module_id)
{
	return 0 <= module_id && module_id < zvm_arrlen(g.modules);
}

#define ZVM_MOD (&g.modules[zvm_arrlen(g.modules)-1])

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

static inline int u32cmp(const uint32_t a, const uint32_t b)
{
	return (a>b)-(b>a);
}

static uint32_t buftop()
{
	return zvm_arrlen(zvm__buf);
}

static uint32_t* bufp(uint32_t p)
{
	return &zvm__buf[p];
}

static inline int get_module_outcome_request_sz(struct module* mod)
{
	const int n_state_bits = 1;
	const int n_output_bits = mod->n_outputs;
	return n_state_bits + n_output_bits;
}

static inline int module_has_state(struct module* mod)
{
	return mod->n_bits > 0;
}

static struct module* get_instance_mod_for_code(uint32_t code)
{
	const int op = code & ZVM_OP_MASK;
	zvm_assert(op == ZVM_OP(INSTANCE) && "expected op to be an instance");
	const int instance_module_id = code >> ZVM_OP_BITS;
	zvm_assert(is_valid_module_id(instance_module_id));
	return &g.modules[instance_module_id];
}


static struct module* get_instance_mod_at_p(uint32_t p)
{
	return get_instance_mod_for_code(*bufp(p));
}

void zvm_init()
{
	zvm_assert(ZVM_OP_N <= ZVM_OP_MASK);
	//memset(ZVM, 0, sizeof(*ZVM));
	memset(&g, 0, sizeof(g));
}

void zvm_begin_program()
{
}

int zvm_begin_module(int n_inputs, int n_outputs)
{
	int id = zvm_arrlen(g.modules);
	struct module m = {0};
	m.n_inputs = n_inputs;
	m.n_outputs = n_outputs;
	m.code_begin_p = buftop();
	zvm_arrpush(g.modules, m);

	return id;
}

static inline int bs32_n_words(int n_bits)
{
	return (n_bits + 31) >> 5;
}

static inline int bs32_n_bytes(int n)
{
	return sizeof(uint32_t) * bs32_n_words(n);
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

static inline void bs32_union_inplace(int n, uint32_t* dst, uint32_t* src)
{
	const int n_words = bs32_n_words(n);
	for (int i = 0; i < n_words; i++) {
		dst[i] |= src[i];
	}
}

#if 0
static inline int bs32_equal(int n, uint32_t* a, uint32_t* b)
{
	for (; n>32; a++,b++,n-=32) if (*a != *b) return 0;
	if ((*a & bs32_mask(n)) != (*b & bs32_mask(n))) return 0;
	return 1;
}

static inline void bs32_sub_inplace(int n, uint32_t* dst, uint32_t* src)
{
	const int n_words = bs32_n_words(n);
	for (int i = 0; i < n_words; i++) {
		dst[i] &= ~src[i];
	}
}

static inline int bs32_popcnt(int n, uint32_t* bs)
{
	int popcnt = 0;
	for (int i = 0; i < n; i++) if (bs32_test(bs, i)) popcnt++;
	return popcnt;
}
#endif

static inline void bs32_fill(int n, uint32_t* bs, int v)
{
	memset(bs, v?~0:0, bs32_n_bytes(n));
}

static inline void bs32_clear_all(int n, uint32_t* bs)
{
	bs32_fill(n, bs, 0);
}

#if 0
static inline void bs32_copy(int n, uint32_t* dst, uint32_t* src)
{
	memcpy(dst, src, bs32_n_bytes(n));
}

static inline void bs32_intersection_inplace(int n, uint32_t* dst, uint32_t* src)
{
	for (; n>32; dst++,src++,n-=32) *dst &= *src;
	(*dst) &= *src | ~bs32_mask(n);
}
#endif

static int bs32_cmp(int n, uint32_t* a, uint32_t* b)
{
	const int n_words = bs32_n_words(n);

	const uint32_t mask = bs32_mask(n&31);
	int c0 = u32cmp(a[n_words-1]&mask, b[n_words-1]&mask);
	if (c0 != 0) return c0;
	for (int i = n_words-2; i >= 0; i--) {
		int c1 = u32cmp(a[i], b[i]);
		if (c1 != 0) return c1;
	}
	return 0;
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

static uint32_t bs32_alloc(int n)
{
	const int n_words = bs32_n_words(n);
	uint32_t* bs = zvm_arradd(g.bs32s, n_words);
	bs32_clear_all(n, bs);
	return bs - g.bs32s;
}

// alloc `n` bitsets, each with `bc` bits. the difference from bs32_alloc(n*nc)
// is alignment; with bs32_alloc_2d() each bitset starts at a uint32_t
// boundary.
static uint32_t bs32_alloc_2d(int n, int bc)
{
	const int n_words_per_bitset = bs32_n_words(bc);
	const int n_words_total = n * n_words_per_bitset;
	uint32_t* bs = zvm_arradd(g.bs32s, n_words_total);
	memset(bs, 0, n_words_total * sizeof(*bs));
	return bs - g.bs32s;
}

static uint32_t* bs32p(int index)
{
	return &g.bs32s[index];
}

uint32_t bs32s_saved_len;
static void bs32s_save_len()
{
	bs32s_saved_len = zvm_arrlen(g.bs32s);
}

static void bs32s_restore_len()
{
	zvm_arrsetlen(g.bs32s, bs32s_saved_len);
}

static inline int n_input_bs32_words(struct module* mod)
{
	return bs32_n_words(mod->n_inputs);
}

static uint32_t* get_input_bs32(struct module* mod, int index)
{
	return bs32p(mod->input_bs32i + index * n_input_bs32_words(mod));
}

static uint32_t* get_state_input_dep_bs32(struct module* mod)
{
	return get_input_bs32(mod, 0);
}

static uint32_t* get_output_input_dep_bs32(struct module* mod, int output_index)
{
	assert(0 <= output_index && output_index < mod->n_outputs);
	return get_input_bs32(mod, 1+output_index);
}

static uint32_t* get_outcome_index_input_dep_bs32(struct module* mod, uint32_t outcome_index)
{
	if (outcome_index == ZVM_NIL_ID) {
		return get_state_input_dep_bs32(mod);
	} else {
		return get_output_input_dep_bs32(mod, outcome_index);
	}
}

static inline int get_op_n_args(uint32_t code)
{
	int op = code & ZVM_OP_MASK;
	if (op == ZVM_OP(INSTANCE)) {
		return get_instance_mod_for_code(code)->n_inputs;
	}
	switch (op) {
	#define DEFOP(op,narg) case ZVM_OP(op): zvm_assert(narg >= 0); return narg;
	ZVM_OPS
	#undef DEFOP
	default: zvm_assert(!"unhandled op"); return 0;
	}
}

#if 0
static inline int zvm__is_valid_arg_index(uint32_t x, int index)
{
	return 0 <= index && index < get_op_n_args(zvm__buf[x]);
}
#endif

static int get_op_length(uint32_t p)
{
	return 1+get_op_n_args(*bufp(p));
}

static int get_op_n_outputs(uint32_t p)
{
	uint32_t code = *bufp(p);
	int op = code & ZVM_OP_MASK;
	if (op == ZVM_OP(INSTANCE)) {
		return get_instance_mod_for_code(code)->n_outputs;
	}

	return 1;
}

static int nodecmp(const void* va, const void* vb)
{
	const struct node_output* a = va;
	const struct node_output* b = vb;
	int c0 = u32cmp(a->p, b->p);
	if (c0 != 0) return c0;
	return u32cmp(a->index, b->index);
}

static int get_node_index(struct module* mod, struct node_output* k)
{
	struct node_output* nodes = &g.node_outputs[mod->node_outputs_i];
	int left = 0;
	int right = mod->n_node_outputs - 1;
	while (left <= right) {
		int mid = (left+right) >> 1;
		int cmp = nodecmp(&nodes[mid], k);
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

uint32_t* get_node_output_bs32(struct module* mod)
{
	return bs32p(mod->node_output_bs32i);
}

static void clear_node_visit_set(struct module* mod)
{
	bs32_clear_all(mod->n_node_outputs, get_node_output_bs32(mod));
}

static int visit_node(struct module* mod, uint32_t p, uint32_t output_index)
{
	int node_index = get_node_index(mod, &((struct node_output) { .p = p, .index = output_index}));
	uint32_t* node_bs32 = bs32p(mod->node_output_bs32i);
	if (bs32_test(node_bs32, node_index)) {
		return 0;
	} else {
		bs32_set(node_bs32, node_index);
		return 1;
	}
}

struct tracer {
	struct module* mod;
	uint32_t substance_id;

	void(*module_input_visitor)(struct tracer*, uint32_t p);
	void(*instance_output_visitor)(struct tracer*, uint32_t p, int output_index);

	int break_at_instance;

	void* usr;
};

static void trace(struct tracer* tr, uint32_t p)
{
	int unpack_index = -1;

	for (;;) {
		if (ZVM_IS_SPECIALX(p, ZVM_X_CONST)) {
			zvm_assert(unpack_index == -1);
			return;
		}

		if (ZVM_IS_SPECIALX(p, ZVM_X_INPUT)) {
			zvm_assert(unpack_index == -1);
			if (tr->module_input_visitor != NULL) {
				tr->module_input_visitor(tr, p);
			}
			return;
		}

		assert(!ZVM_IS_SPECIAL(p));

		zvm_assert((tr->mod->code_begin_p <= p && p < tr->mod->code_end_p) && "p out of range");

		int output_index = unpack_index >= 0 ? unpack_index : 0;

		if (!visit_node(tr->mod, p, output_index)) {
			return;
		}

		uint32_t code = *bufp(p);

		int op = code & ZVM_OP_MASK;
		if (op == ZVM_OP(INSTANCE)) {
			struct module* instance_mod = get_instance_mod_for_code(code);
			if (instance_mod->n_outputs == 1 && unpack_index == -1) unpack_index = 0;
			zvm_assert(0 <= unpack_index && unpack_index < instance_mod->n_outputs && "expected valid unpack");

			if (tr->instance_output_visitor != NULL) {
				tr->instance_output_visitor(tr, p, unpack_index);
			}

			if (!tr->break_at_instance) {
				for (int i = 0; i < instance_mod->n_inputs; i++) {
					if (bs32_test(get_output_input_dep_bs32(instance_mod, unpack_index), i)) {
						trace(tr, *bufp(zvm__arg_index(p, i)));
					}
				}
			}

			return;
		}

		zvm_assert(unpack_index == -1 && "unexpected unpack");

		if (op == ZVM_OP(UNPACK)) {
			unpack_index = code >> ZVM_OP_BITS;
			p = *bufp(zvm__arg_index(p, 0));
			continue;
		} else if (op == ZVM_OP(UNIT_DELAY)) {
			// unit delays have no dependencies
			return;
		} else {
			int n_args = get_op_n_args(code);
			for (int i = 0; i < n_args; i++) {
				trace(tr, *bufp(zvm__arg_index(p, i)));
			}
			return;
		}
	}
}

static void trace_inputs_rec_module_input_visitor(struct tracer* tr, uint32_t p)
{
	uint32_t* input_bs32 = (uint32_t*)tr->usr;
	bs32_set(input_bs32, ZVM_GET_SPECIALY(p));
}

static void trace_inputs_rec(struct module* mod, uint32_t* input_bs32, uint32_t p)
{
	struct tracer tr = {
		.mod = mod,
		.usr = input_bs32,
		.module_input_visitor = trace_inputs_rec_module_input_visitor,
	};
	trace(&tr, p);
}

static void trace_inputs(struct module* mod, uint32_t* input_bs32, uint32_t p)
{
	clear_node_visit_set(mod);
	trace_inputs_rec(mod, input_bs32, p);
}

static void trace_state_deps(uint32_t* input_bs32, struct module* mod)
{
	clear_node_visit_set(mod);

	uint32_t p = mod->code_begin_p;
	const uint32_t p_end = mod->code_end_p;
	while (p < p_end) {
		uint32_t code = *bufp(p);
		int op = code & ZVM_OP_MASK;
		if (op == ZVM_OP(UNIT_DELAY)) {
			trace_inputs_rec(mod, input_bs32, *bufp(zvm__arg_index(p,0)));
		} else if (op == ZVM_OP(INSTANCE)) {
			struct module* instance_mod = get_instance_mod_for_code(code);
			uint32_t* ibs = get_state_input_dep_bs32(instance_mod);
			int n_inputs = instance_mod->n_inputs;
			for (int i = 0; i < n_inputs; i++) {
				if (bs32_test(ibs, i)) {
					trace_inputs_rec(mod, input_bs32, *bufp(zvm__arg_index(p,i)));
				}
			}
		}
		p += get_op_length(p);
	}
	zvm_assert(p == p_end);
}

static uint32_t module_alloc_node_output_bs32(struct module* mod)
{
	return bs32_alloc(mod->n_node_outputs);
}

int zvm_end_module()
{
	struct module* mod = ZVM_MOD;

	mod->code_end_p = buftop();

	struct output* outputs = zvm_arradd(g.outputs, mod->n_outputs);
	mod->outputs_i = outputs - g.outputs;
	for (int i = 0; i < mod->n_outputs; i++) {
		struct output* o = &outputs[i];
		memset(o, 0, sizeof *o);
		o->p = ZVM_PLACEHOLDER;
	}

	struct node_output* np = NULL;

	mod->unit_delay_index_map_i = zvm_arrlen(g.unit_delay_index_maps);

	for (int pass = 0; pass < 2; pass++) {
		int n_nodes_total = 0;

		uint32_t p = mod->code_begin_p;
		const uint32_t p_end = mod->code_end_p;
		while (p < p_end) {
			int n_node_outputs = get_op_n_outputs(p);

			// setup outputs and n_bits
			if (pass == 0) {
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;

				if (op == ZVM_OP(OUTPUT)) {
					int output_index = code >> ZVM_OP_BITS;
					zvm_assert(0 <= output_index && output_index < mod->n_outputs);
					struct output* output = &g.outputs[mod->outputs_i + output_index];
					zvm_assert((output->p == ZVM_PLACEHOLDER) && "double assignment");
					output->p = *bufp(zvm__arg_index(p, 0));
				} else if (op == ZVM_OP(UNIT_DELAY)) {
					mod->n_bits++; // XXX might not be connected?
					struct unit_delay_index x = {
						.p = p,
						.index = mod->unit_delay_index_map_n++,
					};
					zvm_arrpush(g.unit_delay_index_maps, x);
				} else if (op == ZVM_OP(INSTANCE)) {
					struct module* instance_mod = get_instance_mod_for_code(code);
					mod->n_bits += instance_mod->n_bits; // XXX might not be connected?
				}
			}

			if (pass == 1) {
				for (int index = 0; index < n_node_outputs; index++) {
					np->p = p;
					np->index = index;
					np++;
				}
			}

			n_nodes_total += n_node_outputs;
			p += get_op_length(p);
		}
		zvm_assert(p == p_end);

		if (pass == 0) {
			mod->n_node_outputs = n_nodes_total;
			np = zvm_arradd(g.node_outputs, mod->n_node_outputs);
			mod->node_outputs_i = np - g.node_outputs;
			mod->node_output_map_i = zvm_arradd(g.node_output_maps, mod->n_node_outputs) - g.node_output_maps;
		} else if (pass == 1) {
			// qsort not necessary; nodes are inserted in ascending
			// order
		}
	}

	mod->node_output_bs32i = module_alloc_node_output_bs32(mod);

	// initialize input bitsets
	{
		const int n_input_bs32s = 1 + mod->n_inputs;
		mod->input_bs32i = bs32_alloc_2d(n_input_bs32s, mod->n_inputs);
	}

	const int module_id = zvm_arrlen(g.modules) - 1;

	// trace state input-dependencies
	uint32_t* state_input_dep_bs32   = get_state_input_dep_bs32(mod);
	trace_state_deps(state_input_dep_bs32, mod);
	#ifdef VERBOSE_DEBUG
	printf("MODULE %d\n", module_id);
	printf("state: "); bs32_print(mod->n_inputs, state_input_dep_bs32); printf("\n");
	#endif

	// trace output input-dependencies
	for (int i = 0; i < mod->n_outputs; i++) {
		uint32_t* output_input_dep_bs32 = get_output_input_dep_bs32(mod, i);
		trace_inputs(mod, output_input_dep_bs32, g.outputs[mod->outputs_i + i].p);
		#ifdef VERBOSE_DEBUG
		printf("o[%d]: ", i); bs32_print(mod->n_inputs, output_input_dep_bs32); printf("\n");
		#endif
	}
	#ifdef VERBOSE_DEBUG
	printf("\n");
	#endif

	return module_id;
}

static uint32_t node_output_map_get(struct module* mod, struct node_output* k)
{
	return g.node_output_maps[mod->node_output_map_i + get_node_index(mod, k)];
}

static void node_output_map_set(struct module* mod, struct node_output* k, uint32_t value)
{
	g.node_output_maps[mod->node_output_map_i + get_node_index(mod, k)] = value;
}

static void node_output_map_fill(struct module* mod, uint32_t value)
{
	uint32_t* xs = &g.node_output_maps[mod->node_output_map_i];
	const int n = mod->n_node_outputs;
	for (int i = 0; i < n; i++) xs[i] = value;
}

static int substance_key_cmp(const void* va, const void* vb)
{
	const struct substance_key* a = va;
	const struct substance_key* b = vb;

	int c0 = u32cmp(a->module_id, b->module_id);
	if (c0 != 0) return c0;

	struct module* mod = &g.modules[a->module_id];
	const int outcome_request_sz = get_module_outcome_request_sz(mod);

	if (a->outcome_request_bs32i != b->outcome_request_bs32i) {
		return bs32_cmp(outcome_request_sz, &g.bs32s[a->outcome_request_bs32i], &g.bs32s[b->outcome_request_bs32i]);
	} else {
		return 0;
	}
}


static inline int outcome_request_state_index()
{
	return 0;
}

static inline int outcome_request_output_index(int output_index)
{
	return 1+output_index;
}

static inline int outcome_request_state_test(uint32_t bs32i)
{
	return bs32_test(bs32p(bs32i), outcome_request_state_index());
}

static inline int outcome_request_output_test(uint32_t bs32i, int output_index)
{
	return bs32_test(bs32p(bs32i), outcome_request_output_index(output_index));
}

static void outcome_request_state_set(uint32_t bs32i)
{
	bs32_set(bs32p(bs32i), outcome_request_state_index());
}

static void outcome_request_output_set(uint32_t bs32i, int output_index)
{
	bs32_set(bs32p(bs32i), outcome_request_output_index(output_index));
}

static int produce_substance_id_for_key(struct substance_key* key, int* did_insert)
{
	// leftmost binary search; finds either an existing key (in which case,
	// don't insert), or the proper insertion index
	int left = 0;
	int n = zvm_arrlen(g.substance_keyvals);
	int right = n;
	while (left < right) {
		int mid = (left+right) >> 1;
		if (substance_key_cmp(&g.substance_keyvals[mid].key, key) < 0) {
			left = mid + 1;
		} else {
			right = mid;
		}
	}

	if (left < n) {
		struct substance_keyval* keyval = &g.substance_keyvals[left];
		if (substance_key_cmp(&keyval->key, key) == 0) {
			return keyval->substance_id;
		}
	}

	// grow array by one
	(void)zvm_arradd(g.substance_keyvals, 1);

	struct substance_keyval* keyval = &g.substance_keyvals[left];

	int to_move = n - left;
	if (to_move > 0) {
		memmove(keyval+1, keyval, to_move*sizeof(*keyval));
	}

	keyval->key = *key;
	keyval->substance_id = zvm_arrlen(g.substances);

	struct substance sb = {0};
	sb.key = *key;
	#if 0
	fn_init_instance_u32_map(&fn);
	#endif
	zvm_arrpush(g.substances, sb);

	if (did_insert) *did_insert = 1;

	return keyval->substance_id;
}


static int drout_compar(const void* va, const void* vb)
{
	const struct drout* a = va;
	const struct drout* b = vb;
	int c0 = u32cmp(a->p, b->p);
	if (c0 != 0) return c0;
	return u32cmp(a->index, b->index);
}

static void setup_drout(struct drout* drout, uint32_t p, uint32_t index)
{
	memset(drout, 0, sizeof *drout);
	drout->p = p;
	drout->index = index;
}

static void push_drain(uint32_t p, uint32_t index)
{
	setup_drout(zvm_arradd(g.tmp_drains, 1), p, index);
}

static void push_outcome(uint32_t p, uint32_t index)
{
	setup_drout(zvm_arradd(g.tmp_outcomes, 1), p, index);
}

static int drout_find_index(struct drout* drouts, int n, uint32_t kp, uint32_t kidx)
{
	int left = 0;
	int right = n;

	struct drout k = { .p = kp, .index = kidx };
	while (left <= right) {
		int mid = (left+right) >> 1;
		int cmp = drout_compar(&drouts[mid], &k);
		if (cmp < 0) {
			left = mid + 1;
		} else if (cmp > 0) {
			right = mid - 1;
		} else {
			return mid;
		}
	}

	zvm_assert(!"drout not found");
}

static struct drout* drout_find(struct drout* drouts, int n, uint32_t kp, uint32_t ki)
{
	return drouts + drout_find_index(drouts, n, kp, ki);
}

static void add_drain_instance_output_visitor(struct tracer* tr, uint32_t p, int output_index)
{
	struct module* instance_mod = get_instance_mod_at_p(p);
	for (int i = 0; i < instance_mod->n_inputs; i++) {
		if (!bs32_test(get_output_input_dep_bs32(instance_mod, output_index), i)) {
			continue;
		}
		push_drain(p, i);
	}
}

static inline struct substance* resolve_substance_id(uint32_t substance_id)
{
	return &g.substances[substance_id];
}

static void add_drain(uint32_t substance_id, uint32_t p, int index)
{
	push_drain(p, index);

	struct tracer tr = {
		.mod = &g.modules[resolve_substance_id(substance_id)->key.module_id],
		.instance_output_visitor = add_drain_instance_output_visitor
	};
	uint32_t pp = (p == ZVM_NIL_P)
		? g.outputs[tr.mod->outputs_i + index].p
		: *bufp(zvm__arg_index(p, index));
	trace(&tr, pp);
}

static void drain_to_output_instance_output_visitor_count(struct tracer* tr, uint32_t p, int output_index)
{
	struct drout* drain = tr->usr;
	drain->counter++;
	struct drout* outcome = drout_find(g.tmp_outcomes, zvm_arrlen(g.tmp_outcomes), p, output_index);
	outcome->decr_list_n++;
}

static void drain_to_output_instance_output_visitor_write(struct tracer* tr, uint32_t p, int output_index)
{
	struct drout* outcome = drout_find(g.tmp_outcomes, zvm_arrlen(g.tmp_outcomes), p, output_index);
	g.tmp_decr_lists[outcome->decr_list_i + outcome->decr_list_n++] = *(uint32_t*)tr->usr;
}

#define ENCODE_DRAIN(v)    ((v)&0x7fffffff)
#define ENCODE_OUTCOME(v)  ((v)|0x80000000)
#define GET_VALUE(v)       ((v)&0x7fffffff)
#define IS_DRAIN(v)        (((v)&0x80000000)==0)
#define IS_OUTCOME(v)      !IS_DRAIN(v)

static int queue_drain_outcome_compar(const void* va, const void* vb)
{
	uint32_t a = *(uint32_t*)va;
	uint32_t b = *(uint32_t*)vb;
	int c0 = IS_OUTCOME(a) - IS_OUTCOME(b);
	if (c0 != 0) return c0;
	return a-b;
}


static int queue_outcome_pi_compar(const void* va, const void* vb)
{
	uint32_t qa = *(uint32_t*)va;
	uint32_t qb = *(uint32_t*)vb;
	zvm_assert(IS_OUTCOME(qa));
	zvm_assert(IS_OUTCOME(qb));
	uint32_t a = GET_VALUE(qa);
	uint32_t b = GET_VALUE(qb);

	return drout_compar(
		&g.tmp_outcomes[a],
		&g.tmp_outcomes[b]
	);
}

static int queue_outcome_closing_compar(const void* va, const void* vb)
{
	uint32_t qa = *(uint32_t*)va;
	uint32_t qb = *(uint32_t*)vb;
	zvm_assert(IS_OUTCOME(qa));
	zvm_assert(IS_OUTCOME(qb));
	uint32_t a = GET_VALUE(qa);
	uint32_t b = GET_VALUE(qb);
	struct drout* da = &g.tmp_outcomes[a];
	struct drout* db = &g.tmp_outcomes[b];
	int c0 = db->usr - da->usr;
	if (c0 != 0) return c0;
	return drout_compar(da, db);
}

static int queue_outcome_get_pspan_length(uint32_t* queue, int n, int break_on_drain)
{
	uint32_t p0 = ZVM_NIL_P;
	for (int i = 0; i < n; i++) {
		uint32_t qv = queue[i];
		int is_outcome = IS_OUTCOME(qv);
		if (break_on_drain) {
			if (!is_outcome) {
				return i;
			}
		} else {
			zvm_assert(is_outcome);
		}
		struct drout* outcome = &g.tmp_outcomes[GET_VALUE(qv)];
		if (p0 == ZVM_NIL_P) {
			p0 = outcome->p;
		} else if (outcome->p != p0) {
			return i;
		}
	}
	return n;
}

static void push_step(uint32_t p, uint32_t substance_id)
{
	struct step step = { .p = p, .substance_id = substance_id };
	zvm_arrpush(g.steps, step);
}

static int ack_substance(uint32_t p, uint32_t queue_i, int n, int* queue_np, int lookup_only)
{
	bs32s_save_len();

	uint32_t code = *bufp(p);

	int instance_module_id = code >> ZVM_OP_BITS;
	zvm_assert(is_valid_module_id(instance_module_id));

	struct module* instance_mod = &g.modules[instance_module_id];
	const int outcome_request_sz = get_module_outcome_request_sz(instance_mod);

	struct substance_key key = {
		.module_id = instance_module_id,
		.outcome_request_bs32i = bs32_alloc(outcome_request_sz),
	};

	uint32_t* queue = &g.tmp_queue[queue_i];
	for (int i = 0; i < n; i++) {
		// populate outcome_request_bs32_p ...
		uint32_t qv = queue[i];
		zvm_assert(IS_OUTCOME(qv));
		uint32_t outcome_index = GET_VALUE(qv);
		struct drout* outcome = &g.tmp_outcomes[outcome_index];
		zvm_assert(outcome->p == p);
		if (outcome->index == ZVM_NIL_ID) {
			outcome_request_state_set(key.outcome_request_bs32i);
		} else {
			outcome_request_output_set(key.outcome_request_bs32i, outcome->index);
		}

		if (!lookup_only) {
			// potentially release outcomes ...
			uint32_t decr_list_n = outcome->decr_list_n;
			uint32_t decr_list_i = outcome->decr_list_i;
			for (int i = 0; i < decr_list_n; i++) {
				uint32_t drain_index = g.tmp_decr_lists[decr_list_i + i];
				struct drout* drain = &g.tmp_drains[drain_index];
				zvm_assert(drain->counter > 0 && "decrement when zero not expected");
				drain->counter--;
				if (drain->counter == 0) {
					g.tmp_queue[(*queue_np)++] = ENCODE_DRAIN(drain_index);
				}
			}
		}
	}

	int did_insert = 0;
	uint32_t produced_substance_id = produce_substance_id_for_key(&key, &did_insert);

	zvm_assert((lookup_only == 0 || did_insert == 0) && "not expecting insert for lookup-only calls");

	if (!did_insert) {
		// no insert; rollback allocations
		bs32s_restore_len();
	}

	return produced_substance_id;
}

static void process_substance(uint32_t substance_id)
{
	struct substance_key key = resolve_substance_id(substance_id)->key;
	struct module* mod = &g.modules[key.module_id];

	clear_node_visit_set(mod);

	zvm_arrsetlen(g.tmp_drains, 0);
	zvm_arrsetlen(g.tmp_outcomes, 0);

	// find drains ...
	{
		for (int output_index = 0; output_index < mod->n_outputs; output_index++) {
			if (!outcome_request_output_test(key.outcome_request_bs32i, output_index)) {
				continue;
			}
			add_drain(substance_id, ZVM_NIL_P, output_index);
		}

		if (outcome_request_state_test(key.outcome_request_bs32i)) {
			uint32_t p = mod->code_begin_p;
			const uint32_t p_end = mod->code_end_p;
			while (p < p_end) {
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;
				if (op == ZVM_OP(UNIT_DELAY)) {
					add_drain(substance_id, p, 0);
				} else if (op == ZVM_OP(INSTANCE)) {
					struct module* instance_mod = get_instance_mod_for_code(code);
					if (module_has_state(instance_mod)) {
						int n_inputs = instance_mod->n_inputs;
						uint32_t* ibs = get_state_input_dep_bs32(instance_mod);
						for (int i = 0; i < n_inputs; i++) {
							if (bs32_test(ibs, i)) {
								add_drain(substance_id, p, i);
							}
						}
					}
				}
				p += get_op_length(p);
			}
			zvm_assert(p == p_end);
		}

		// sort and compact drain array by removing duplicates

		const int n_drains_with_dupes = zvm_arrlen(g.tmp_drains);
		qsort(
			g.tmp_drains,
			n_drains_with_dupes,
			sizeof(*g.tmp_drains),
			drout_compar);

		uint32_t read_i = 0;
		uint32_t write_i = 0;
		uint32_t i_end = n_drains_with_dupes;

		while (read_i < i_end) {

			if (read_i != write_i) {
				memcpy(&g.tmp_drains[write_i], &g.tmp_drains[read_i], sizeof(*g.tmp_drains));
			}

			uint32_t i0 = read_i;
			do {
				read_i++;
			} while (read_i < i_end && drout_compar(&g.tmp_drains[i0], &g.tmp_drains[read_i]) == 0);

			write_i++;
		}
		zvm_assert(read_i == i_end);

		zvm_arrsetlen(g.tmp_drains, write_i);
	}

	// find outcomes ...
	{
		// as a side effect of finding drains, node_output_bs32_p has
		// 1's for all node outputs visited; for each instance output,
		// add a drout

		uint32_t* node_bs32 = get_node_output_bs32(mod);
		for (int i = 0; i < mod->n_node_outputs; i++) {
			if (!bs32_test(node_bs32, i)) {
				continue;
			}
			struct node_output* node_output = &g.node_outputs[mod->node_outputs_i + i];
			uint32_t code = *bufp(node_output->p);
			int op = code & ZVM_OP_MASK;
			if (op != ZVM_OP(INSTANCE)) {
				continue;
			}

			push_outcome(node_output->p, node_output->index);
		}

		if (outcome_request_state_test(key.outcome_request_bs32i)) {
			// add state outcome requests
			uint32_t p = mod->code_begin_p;
			const uint32_t p_end = mod->code_end_p;
			while (p < p_end) {
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;
				if (op == ZVM_OP(INSTANCE)) {
					if (module_has_state(get_instance_mod_for_code(code))) {
						push_outcome(p, ZVM_NIL_ID);
					}
				}
				p += get_op_length(p);
			}
			zvm_assert(p == p_end);

			// NOTE: assumption here that drouts only require
			// sorting when request_state is true
			qsort(
				g.tmp_outcomes,
				zvm_arrlen(g.tmp_outcomes),
				sizeof(*g.tmp_outcomes),
				drout_compar);
		}
	}

	// initialize counters and decrement lists

	const int n_drains = zvm_arrlen(g.tmp_drains);
	const int n_outcomes = zvm_arrlen(g.tmp_outcomes);

	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			// reset counters; used for indexing when writing
			// decrement list and are thus reinitialized

			for (int i = 0; i < n_drains; i++) {
				g.tmp_drains[i].decr_list_n = 0;
			}

			for (int i = 0; i < n_outcomes; i++) {
				g.tmp_outcomes[i].decr_list_n = 0;
			}
		}

		for (int i = 0; i < n_drains; i++) {
			struct drout* drain = &g.tmp_drains[i];

			struct tracer tr = {
				.mod = mod,
				.substance_id = substance_id,
				.break_at_instance = 1, // stop at instance outputs
			};
			if (pass == 0) {
				tr.instance_output_visitor = drain_to_output_instance_output_visitor_count;
				tr.usr = drain;
			} else if (pass == 1) {
				tr.instance_output_visitor = drain_to_output_instance_output_visitor_write;
				tr.usr = &i;
			}
			uint32_t p = (drain->p == ZVM_NIL_P)
				? g.outputs[mod->outputs_i + drain->index].p
				: *bufp(zvm__arg_index(drain->p, drain->index));
			clear_node_visit_set(mod);
			trace(&tr, p);
		}

		for (int i = 0; i < n_outcomes; i++) {
			struct drout* outcome = &g.tmp_outcomes[i];
			struct module* instance_mod = get_instance_mod_at_p(outcome->p);

			uint32_t* bs32 = get_outcome_index_input_dep_bs32(instance_mod, outcome->index);
			for (int j = 0; j < instance_mod->n_inputs; j++) {
				if (!bs32_test(bs32, j)) {
					continue;
				}

				struct drout* drain = drout_find(g.tmp_drains, zvm_arrlen(g.tmp_drains), outcome->p, j);

				if (pass == 0) {
					outcome->counter++;
					drain->decr_list_n++;
				} else if (pass == 1) {
					g.tmp_decr_lists[drain->decr_list_i + (drain->decr_list_n++)] = i;
				} else {
					zvm_assert(!"unreachable");
				}
			}
		}

		if (pass == 0) {
			// allocate decrement lists

			uint32_t top = 0;
			for (int i = 0; i < n_drains; i++) {
				struct drout* drain = &g.tmp_drains[i];
				drain->decr_list_i = top;
				top += drain->decr_list_n;
			}

			for (int i = 0; i < n_outcomes; i++) {
				struct drout* outcome = &g.tmp_outcomes[i];
				outcome->decr_list_i = top;
				top += outcome->decr_list_n;
			}

			zvm_arrsetlen(g.tmp_decr_lists, top);
		}
	}

	const int new_substance_ids_begin = zvm_arrlen(g.substances);

	// initialize drain/outcome queue
	const int queue_sz = n_drains+n_outcomes;
	(void)zvm_arrsetlen(g.tmp_queue, queue_sz);

	int queue_i = 0;
	int queue_n = 0;

	for (int i = 0; i < n_drains; i++) {
		struct drout* drain = &g.tmp_drains[i];
		if (drain->counter == 0) {
			g.tmp_queue[queue_n++] = ENCODE_DRAIN(i);
		}

	}

	clear_node_visit_set(mod);

	for (int i = 0; i < n_outcomes; i++) {
		struct drout* outcome = &g.tmp_outcomes[i];
		if (outcome->counter == 0) {
			g.tmp_queue[queue_n++] = ENCODE_OUTCOME(i);
		}

		uint32_t index = outcome->index;
		if (index != ZVM_NIL_ID) {
			// mark instance outcome nodes in visit set;
			// used to detect if a sequence of instance
			// output outcomes constitute a "non-fragmented
			// call"
			visit_node(mod, outcome->p, index);
		}
	}

	while (queue_i < queue_n) {
		int queue_span_n_drains = 0;
		int queue_span_n_outcomes = 0;

		for (int i = queue_i; i < queue_n; i++) {
			uint32_t qv = g.tmp_queue[i];
			if (IS_DRAIN(qv)) queue_span_n_drains++;
			if (IS_OUTCOME(qv)) queue_span_n_outcomes++;
		}

		if (queue_span_n_drains > 0) {
			// keep executing drains, and remove them from queue,
			// as long as they're available...

			// move outcomes to end, if any
			if (queue_span_n_outcomes > 0) {
				qsort(&g.tmp_queue[queue_i], queue_n-queue_i, sizeof(*g.tmp_queue), queue_drain_outcome_compar);
			}

			int queue_n0 = queue_n;
			for (; queue_i < queue_n0; queue_i++) {
				uint32_t qv = g.tmp_queue[queue_i];

				if (IS_OUTCOME(qv)) {
					zvm_assert(queue_span_n_outcomes > 0);
					break;
				}

				// release outcomes ...
				struct drout* drain = &g.tmp_drains[GET_VALUE(qv)];
				const int n = drain->decr_list_n;
				uint32_t* decr_list = &g.tmp_decr_lists[drain->decr_list_i];
				for (int i = 0; i < n; i++) {
					const int outcome_index = decr_list[i];
					struct drout* outcome = &g.tmp_outcomes[outcome_index];
					zvm_assert(outcome->counter > 0);
					outcome->counter--;
					if (outcome->counter == 0) {
						g.tmp_queue[queue_n++] = ENCODE_OUTCOME(outcome_index);
					}
				}
			}

			continue;
		}

		zvm_assert(queue_span_n_drains == 0 && queue_span_n_outcomes > 0);

		// now there are only outcomes in the [queue_i;queue_n]
		// interval.

		qsort(&g.tmp_queue[queue_i], queue_n-queue_i, sizeof(*g.tmp_queue), queue_outcome_pi_compar);

		#if 0
		for (int i = queue_i; i < queue_n; i++) zvm_assert(IS_OUTCOME(queue[i]));
		#endif

		// look for closing substances ...
		int n_closing_substances = 0;
		for (int i = queue_i; i < queue_n; ) {
			const int pspan_length = queue_outcome_get_pspan_length(&g.tmp_queue[i], queue_n-i, 0);

			const uint32_t p0 = g.tmp_outcomes[GET_VALUE(g.tmp_queue[i])].p;

			struct module* instance_mod = get_instance_mod_at_p(p0);

			const int n_instance_outputs = instance_mod->n_outputs;

			uint32_t* node_bs32 = get_node_output_bs32(mod);

			int is_closing_substance = 1;
			int ii = 0;
			int requesting_state = module_has_state(instance_mod) && outcome_request_state_test(key.outcome_request_bs32i);
			int n_checks = n_instance_outputs + (requesting_state ? 1 : 0);
			if (n_checks > (queue_n-queue_i)) {
				// if queue size is smaller than the number of
				// outcomes required for a "closing substance",
				// then it cannot possibly be a "closing
				// substance"
				is_closing_substance = 0;
			} else for (int j = 0; j < n_checks; j++) {
				// a substance is "closing" if the inferred
				// outcome request is identical to the
				// remaining outcome request
				const int is_output = (j < n_instance_outputs);
				const int is_state  = (j == n_instance_outputs);

				uint32_t expected_drout_index = 0;
				if (is_output) {
					int node_index = get_node_index(mod, &((struct node_output) { .p = p0, .index = j}));
					if (!bs32_test(node_bs32, node_index)) {
						continue;
					}
					expected_drout_index = j;
				} else if (is_state) {
					expected_drout_index = ZVM_NIL_ID;
				} else {
					zvm_assert(!"unreachable");
				}

				struct drout* outcome = &g.tmp_outcomes[GET_VALUE(g.tmp_queue[i + (ii++)])];
				zvm_assert(outcome->p == p0);
				if (outcome->index != expected_drout_index) {
					is_closing_substance = 0;
					break;
				}
			}

			if (is_closing_substance) {
				zvm_assert(ii == pspan_length);
			}

			if (is_closing_substance) n_closing_substances++;

			// tag outcomes so qsort can place closing
			// substances in front of queue
			for (int j = 0; j < pspan_length; j++) {
				struct drout* outcome = &g.tmp_outcomes[GET_VALUE(g.tmp_queue[i+j])];
				outcome->usr = is_closing_substance;
			}

			i += pspan_length;
		}

		if (n_closing_substances > 0) {
			// place closing substances in beginning of queue
			qsort(&g.tmp_queue[queue_i], queue_n-queue_i, sizeof(*g.tmp_queue), queue_outcome_closing_compar);

			int queue_n0 = queue_n;
			while (n_closing_substances > 0 && queue_i < queue_n0) {
				const int pspan_length = queue_outcome_get_pspan_length(&g.tmp_queue[queue_i], queue_n0-queue_i, 0);

				const uint32_t p0 = g.tmp_outcomes[GET_VALUE(g.tmp_queue[queue_i])].p;

				ack_substance(
					p0,
					queue_i,
					pspan_length,
					&queue_n,
					0);

				queue_i += pspan_length;
				n_closing_substances--;
			}

			zvm_assert((n_closing_substances == 0) && "expected to handle all closing substances");

		} else {
			// no closing substances to choose from, revealing that
			// an instance split into 2+ substances is unavoidable.
			// it seems complicated to pick the best split (as in
			// O(n!) maybe?)

			// current attempt at a heuristic solution is to simply
			// pick the "top of the queue" and continue :-)

			int pspan_length = queue_outcome_get_pspan_length(&g.tmp_queue[queue_i], queue_n-queue_i, 0);
			uint32_t p0 = g.tmp_outcomes[GET_VALUE(g.tmp_queue[queue_i])].p;

			// non-closing substances are not allowed to request
			// state, so exclude the state request if present. this
			// makes it easier to enforce that state is never
			// written before the last read
			struct drout* last_outcome = &g.tmp_outcomes[GET_VALUE(g.tmp_queue[queue_i + pspan_length - 1])];
			if (last_outcome->index == ZVM_NIL_ID) {
				struct module* instance_mod = get_instance_mod_at_p(p0);
				int requesting_state = module_has_state(instance_mod) && outcome_request_state_test(key.outcome_request_bs32i);
				zvm_assert(requesting_state && "found state outcome but not requesting state?");
				pspan_length--;
			}

			#ifdef DEBUG
			for (int i = 0; i < pspan_length; i++) {
				zvm_assert((g.tmp_outcomes[GET_VALUE(g.tmp_queue[queue_i + i])].index != ZVM_NIL_ID) && "state outcome not allowed at this point");
			}
			#endif

			ack_substance(
				p0,
				queue_i,
				pspan_length,
				&queue_n,
				0);

			queue_i += pspan_length;
		}
	}

	zvm_assert(queue_i == queue_sz);
	zvm_assert(queue_n == queue_sz);

	// go through queue again; this time construct "the sequence"; I'd
	// prefer constructing in the loop above, but ack_substance() *also*
	// mutates buf

	struct substance* sb = resolve_substance_id(substance_id);
	sb->steps_i = zvm_arrlen(g.steps);
	queue_i = 0;
	while (queue_i < queue_n) {
		uint32_t qv = g.tmp_queue[queue_i];
		if (IS_DRAIN(qv)) {
			uint32_t v = GET_VALUE(qv);
			struct drout* drain = &g.tmp_drains[v];
			uint32_t p = drain->p;
			if (p != ZVM_NIL_P) {
				zvm_assert(!ZVM_IS_SPECIAL(p));
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;
				if (op == ZVM_OP(UNIT_DELAY)) {
					push_step(p, ZVM_NIL_ID);
				}
			}
			queue_i++;
			continue;
		}

		int pspan_length = queue_outcome_get_pspan_length(&g.tmp_queue[queue_i], queue_n-queue_i, 1);
		zvm_assert(pspan_length > 0);

		uint32_t p0 = g.tmp_outcomes[GET_VALUE(g.tmp_queue[queue_i])].p;

		uint32_t ack_substance_id = ack_substance(
			p0,
			queue_i,
			pspan_length,
			&queue_n,
			1);

		push_step(p0, ack_substance_id);

		queue_i += pspan_length;
	}
	sb->n_steps = zvm_arrlen(g.steps) - sb->steps_i;

	#if 0
	#ifdef VERBOSE_DEBUG
	printf("Sequence for substance #%d:\n", substance_id);
	for (int i = 0; i < sb->sequence_len; i++) {
		uint32_t* pair = bufp(sb->sequence_p + (i<<1));
		uint32_t p = pair[0];
		uint32_t substance_id = pair[1];
		if (substance_id == ZVM_NIL_ID) {
			printf("  p=%d substance_id=<nil> (unit delay)\n", p);
		} else {
			printf("  p=%d substance_id=%d\n", p, substance_id);
		}
	}
	#endif
	#endif

	const int new_substance_ids_end = zvm_arrlen(g.substances);
	for (int new_substance_id = new_substance_ids_begin; new_substance_id < new_substance_ids_end; new_substance_id++) {
		process_substance(new_substance_id);
	}
}

static void emit_functions_rec(int substance_id)
{
	struct substance* sb = &g.substances[substance_id];

	sb->refcount++;

	if (sb->tag) return;
	sb->tag = 1;

	const int n_steps = sb->n_steps;
	for (int i = 0; i < n_steps; i++) {
		struct step* step = &g.steps[sb->steps_i + i];
		if (step->substance_id == ZVM_NIL_ID) {
			continue;
		}
		emit_functions_rec(step->substance_id);
	}

	struct function fn = {
		.substance_id = substance_id,
	};

	zvm_arrpush(g.functions, fn);

	// TODO can I do some local analysis here...?
	//
	//  - truth table optimization... it's probably not even worth storing
	//    the information because it's something like, "do truth table
	//    optimization if !has_state && n_inputs < 8", or, instead of
	//    basing it on number of inputs, base it on the size of the truth
	//    table... e.g. I can store the truth table for a 4-to-16 decoder
	//    in 256 bits (16 possible inputs; 16 output bits each; 16*16=256).
	//    actually... state doesn't prevent truth table optimization as
	//    long as state bits are counted as inputs? maybe?
	//
	//  - inline-ability? local analysis can reveal the complexity, and
	//    seems somewhat related to truth-table optimization stuff?
	//    however, inline-ability may also be based on top-down analysis
	//    (e.g. if a substance is seen only once, then there's not really
	//    any reason not to inline it, except, maybe, for the important
	//    purpose of emitting bytecode than can be disassembled and made
	//    sense of, but that's like "-O0 -g")
}

static void clear_substance_tags()
{
	const int n_substances = zvm_arrlen(g.substances);
	for (int i = 0; i < n_substances; i++) {
		g.substances[i].tag = 0;
	}
}

static uint32_t fn_trace(struct module* mod, uint32_t p)
{

	return 0; // XXX
}

static int get_unit_delay_index(struct module* mod, uint32_t p)
{
	struct unit_delay_index* xs = &g.unit_delay_index_maps[mod->unit_delay_index_map_i];
	int left = 0;
	int right = mod->unit_delay_index_map_n - 1;
	while (left <= right) {
		int mid = (left+right) >> 1;
		uint32_t t = xs[mid].p;
		if (t < p) {
			left = mid + 1;
		} else if (t > p) {
			right = mid - 1;
		} else {
			return mid;
		}
	}
	zvm_assert(!"unit delay p not found");
}

#define OPS \
	\
	DEFOP(STATEFUL_CALL,3) \
	DEFOP(STATELESS_CALL,2) \
	DEFOP(RETURN,0) \
	DEFOP(ARITH_2_1,3) \
	DEFOP(ARITH_1_1,2) \
	DEFOP(MOVE,2) \
	DEFOP(WRITE,2) \
	DEFOP(READ,2)



#define OP(op) OP_##op

enum ops {
	OP_NIL = 0,
	#define DEFOP(op,narg) OP(op),
	OPS
	#undef DEFOP
	OP_N
};

static void emit_bytecode(uint32_t bc)
{
	zvm_arrpush(g.bytecode, bc);
}

static void emit_state_write(struct module* mod, uint32_t p, uint32_t reg)
{
	int state_index = get_unit_delay_index(mod, p);
	emit_bytecode(OP(WRITE));
	emit_bytecode(state_index);
	emit_bytecode(reg);
}

static void emit_call_stub(uint32_t function_id)
{
	emit_bytecode(0); // XXX
	struct function* fn = &g.functions[function_id];
	emit_bytecode(fn->bytecode_i);
}

static void emit_call_arg(uint32_t reg)
{
	zvm_arrpush(g.bytecode, reg);
}

static uint32_t resolve_function_id_for_substance_id(uint32_t substance_id)
{
	// XXX somewhat naive lookup?
	const int n_functions = zvm_arrlen(g.functions);
	for (int function_id = 0; function_id < n_functions; function_id++) {
		struct function* fn = &g.functions[function_id];
		if (fn->substance_id == substance_id) {
			return function_id;
		}
	}
	zvm_assert(!"substance id not found");
}

static void emit_function_code(uint32_t function_id)
{
	struct function* fn = &g.functions[function_id];
	fn->bytecode_i = zvm_arrlen(g.bytecode);

	struct substance* sb = &g.substances[fn->substance_id];
	struct module* mod = &g.modules[sb->key.module_id];

	//printf("function %d\n", function_id);

	// bum bum...
	//  - jeg tænker at have en register allocator, der egentlig bare er en
	//    counter... så den skal sættes til 0 her?
	//  - node output map...? hvor jeg gemmer registre?

	node_output_map_fill(mod, ZVM_NIL_ID);

	for (int i = 0; i < sb->n_steps; i++) {
		struct step* step = &g.steps[sb->steps_i + i];

		const int is_unit_delay = (step->substance_id == ZVM_NIL_ID);

		if (is_unit_delay) {
			uint32_t reg = fn_trace(mod, *bufp(zvm__arg_index(step->p, 0)));
			emit_state_write(mod, step->p, reg);
		} else {
			struct substance* step_sb = &g.substances[step->substance_id];
			struct module* step_mod = &g.modules[step_sb->key.module_id];
			zvm_assert((step_sb->key.module_id == (*bufp(step->p) >> ZVM_OP_BITS)) && "subkey/op module id mismatch");

			//const int outcome_request_sz = get_module_outcome_request_sz(mod);
			uint32_t outcome_request_bs32i = step_sb->key.outcome_request_bs32i;

			const int n_inputs = step_mod->n_inputs;
			const int n_outputs = step_mod->n_outputs;

			bs32s_save_len();
			uint32_t* input_set_bs32 = bs32p(bs32_alloc(n_inputs));

			// for each requested outcome, add the coresponding
			// input dependencies to the input set
			for (int output_index = 0; output_index < n_outputs; output_index++) {
				if (!outcome_request_output_test(outcome_request_bs32i, output_index)) {
					continue;
				}
				bs32_union_inplace(n_inputs, input_set_bs32, get_output_input_dep_bs32(step_mod, output_index));
			}
			if (outcome_request_state_test(outcome_request_bs32i)) {
				bs32_union_inplace(n_inputs, input_set_bs32, get_state_input_dep_bs32(step_mod));
			}

			// trace twice; second pass will not emit any code,
			// allowing us to emit the result registers as call
			// arguments
			for (int pass = 0; pass < 2; pass++) {
				if (pass == 1) {
					uint32_t call_function_id = resolve_function_id_for_substance_id(step->substance_id);
					zvm_assert((call_function_id < function_id) && "call to function not yet emitted");
					emit_call_stub(function_id);
				}

				for (int input_index = 0; input_index < n_inputs; input_index++) {
					if (!bs32_test(input_set_bs32, input_index)) {
						continue;
					}
					int bytecode_top = zvm_arrlen(g.bytecode);
					uint32_t reg = fn_trace(mod, *bufp(zvm__arg_index(step->p, input_index)));
					if (pass == 1) {
						zvm_assert((bytecode_top == zvm_arrlen(g.bytecode)) && "did not expect bytecode to mutate");
						emit_call_arg(reg);
					}
				}
			}

			// TODO ... I uh... have to... uh... this relates to
			// packing and unpacking :) or uh...

			bs32s_restore_len();
		}
	}
}

static void emit_functions()
{
	clear_substance_tags();
	emit_functions_rec(g.main_substance_id);

	const int n_functions = zvm_arrlen(g.functions);
	for (int i = 0; i < n_functions; i++) {
		emit_function_code(i);
	}

	#if 0
	#ifdef VERBOSE_DEBUG
	const int n_substances = zvm_arrlen(g.substances);
	for (int i = 0; i < n_substances; i++) {
		struct substance* sb = &g.substances[i];
		const int has_state = outcome_request_state_test(sb->key.outcome_request_bs32i);
		printf("analyze; substance=%d; module=%d; state=%d; refcount=%d\n", i, sb->key.module_id, has_state, sb->refcount);
	}
	printf("\n");
	#endif
	#endif
}


void zvm_end_program(uint32_t main_module_id)
{
	int buf_sz_after_end_program = buftop();

	g.main_module_id = main_module_id;
	struct module* mod = &g.modules[main_module_id];

	const int outcome_request_sz = get_module_outcome_request_sz(mod);

	struct substance_key main_key = {
		.module_id = main_module_id,
		.outcome_request_bs32i = bs32_alloc(outcome_request_sz),
	};

	bs32_fill(outcome_request_sz, &g.bs32s[main_key.outcome_request_bs32i], 1);

	int did_insert = 0;
	process_substance(g.main_substance_id = produce_substance_id_for_key(&main_key, &did_insert));
	zvm_assert(did_insert);

	emit_functions();

	// have a look at
	// https://compileroptimizations.com/
	// to find inspiration, maybe

	#ifdef VERBOSE_DEBUG
	printf("=======================================\n");
	const int n_substances = zvm_arrlen(g.substances);
	printf("n_substances: %d\n", n_substances);
	for (int i = 0; i < n_substances; i++) {
		struct substance* sb = &g.substances[i];
		struct substance_key* key = &sb->key;
		printf("   SB[%d] :: module_id=%d", i , key->module_id);

		const int outcome_request_sz = get_module_outcome_request_sz(&g.modules[key->module_id]);

		printf(" rq=");
		bs32_print(outcome_request_sz, bufp(key->outcome_request_bs32i));

		printf(" n_steps=%d", sb->n_steps);

		printf("\n");
	}
	printf("buf sz after end program:        %d\n", buf_sz_after_end_program);
	printf("=======================================\n");
	#endif
}
