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

static inline int u32cmp(const uint32_t a, const uint32_t b)
{
	return (a>b)-(b>a);
}

static inline int u32paircmp(const uint32_t* a, const uint32_t* b)
{
	int c0 = u32cmp(a[0], b[0]);
	if (c0 != 0) return c0;
	return u32cmp(a[1], b[1]);
}

static uint32_t buftop()
{
	return zvm_arrlen(ZVM_PRG->buf);
}

static uint32_t* bufp(uint32_t p)
{
	return &ZVM_PRG->buf[p];
}

static inline int get_module_drain_request_sz(struct zvm_module* mod)
{
	const int n_state_bits = 1;
	const int n_output_bits = mod->n_outputs;
	return n_state_bits + n_output_bits;
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

static inline int bs32_equal(int n, uint32_t* a, uint32_t* b)
{
	for (; n>32; a++,b++,n-=32) if (*a != *b) return 0;
	if ((*a & bs32_mask(n)) != (*b & bs32_mask(n))) return 0;
	return 1;
}

static inline void bs32_union_inplace(int n, uint32_t* dst, uint32_t* src)
{
	const int n_words = bs32_n_words(n);
	for (int i = 0; i < n_words; i++) {
		dst[i] |= src[i];
	}
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


static inline void bs32_fill(int n, uint32_t* bs, int v)
{
	memset(bs, v?~0:0, bs32_n_bytes(n));
}

static inline void bs32_clear_all(int n, uint32_t* bs)
{
	bs32_fill(n, bs, 0);
}

static inline void bs32_copy(int n, uint32_t* dst, uint32_t* src)
{
	memcpy(dst, src, bs32_n_bytes(n));
}

static inline void bs32_intersection_inplace(int n, uint32_t* dst, uint32_t* src)
{
	for (; n>32; dst++,src++,n-=32) *dst &= *src;
	(*dst) &= *src | ~bs32_mask(n);
}

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
	uint32_t p = buftop();
	uint32_t* bs = zvm_arradd(ZVM_PRG->buf, bs32_n_words(n));
	bs32_clear_all(n, bs);
	return p;
}

static inline int mod_n_input_bs32_words(struct zvm_module* mod)
{
	return bs32_n_words(mod->n_inputs);
}

static uint32_t* get_input_bs32(struct zvm_module* mod, int index)
{
	return bufp(mod->input_bs32s_p + index * mod_n_input_bs32_words(mod));
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
	return 1+zvm__op_n_args(*bufp(p));
}

static int get_op_n_outputs(uint32_t p)
{
	uint32_t code = *bufp(p);
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
	return u32paircmp(a, b);
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
			int n_node_outputs = get_op_n_outputs(p);

			if (pass == 1) {
				for (int i = 0; i < n_node_outputs; i++) {
					//printf("->%d:%d\n",p,i);
					*(np++) = p;
					*(np++) = i;
				}
			}

			n_nodes_total += n_node_outputs;
			p += get_op_length(p);
		}
		zvm_assert(p == p_end);

		if (pass == 0) {
			mod->n_node_outputs = n_nodes_total;
			mod->node_outputs_p = buftop();
			np = nodes = zvm_arradd(ZVM_PRG->buf, 2*mod->n_node_outputs);
		} else if (pass == 1) {
			// qsort not necessary; nodes are inserted in ascending
			// order
		}
	}
}

static int get_node_index(struct zvm_module* mod, uint32_t p, uint32_t i)
{
	uint32_t* nodes = bufp(mod->node_outputs_p);
	const uint32_t k[] = {p,i};
	int left = 0;
	int right = mod->n_node_outputs - 1;
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

uint32_t* get_node_output_bs32(struct zvm_module* mod)
{
	return bufp(mod->node_output_bs32_p);
}

static void clear_node_visit_set(struct zvm_module* mod)
{
	bs32_clear_all(mod->n_node_outputs, get_node_output_bs32(mod));
}

static int visit_node(struct zvm_module* mod, uint32_t p, uint32_t output_index)
{
	int node_index = get_node_index(mod, p, output_index);
	uint32_t* node_bs32 = get_node_output_bs32(mod);
	if (bs32_test(node_bs32, node_index)) {
		return 0;
	} else {
		bs32_set(node_bs32, node_index);
		return 1;
	}
}

struct tracer {
	struct zvm_module* mod;
	uint32_t function_id;

	void(*module_input_visitor)(struct tracer*, uint32_t p);
	void(*instance_output_visitor)(struct tracer*, uint32_t p, int output_index);

	int break_at_instance;

	void* usr;
};

static inline struct zvm_function* tracer_function(struct tracer* tr)
{
	return &ZVM_PRG->functions[tr->function_id];
}

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
			int module_id = code >> ZVM_OP_BITS;
			zvm_assert(zvm__is_valid_module_id(module_id));
			struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];
			if (mod2->n_outputs == 1 && unpack_index == -1) unpack_index = 0;
			zvm_assert(0 <= unpack_index && unpack_index < mod2->n_outputs && "expected valid unpack");

			if (tr->instance_output_visitor != NULL) {
				tr->instance_output_visitor(tr, p, unpack_index);
			}

			if (!tr->break_at_instance) {
				for (int i = 0; i < mod2->n_inputs; i++) {
					if (bs32_test(get_output_input_dep_bs32(mod2, unpack_index), i)) {
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
			int n_args = zvm__op_n_args(code);
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

static void trace_inputs_rec(struct zvm_module* mod, uint32_t* input_bs32, uint32_t p)
{
	struct tracer tr = {
		.mod = mod,
		.usr = input_bs32,
		.module_input_visitor = trace_inputs_rec_module_input_visitor,
	};
	trace(&tr, p);
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
		uint32_t code = *bufp(p);
		int op = code & ZVM_OP_MASK;
		if (op == ZVM_OP(UNIT_DELAY)) {
			trace_inputs_rec(mod, input_bs32, *bufp(zvm__arg_index(p,0)));
		} else if (op == ZVM_OP(INSTANCE)) {
			int module_id = code >> ZVM_OP_BITS;
			zvm_assert(zvm__is_valid_module_id(module_id));
			struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];
			uint32_t* ibs = get_state_input_dep_bs32(mod2);
			int n_inputs = mod2->n_inputs;
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

int zvm_end_module()
{
	struct zvm_module* mod = ZVM_MOD;

	mod->code_end_p = buftop();

	build_node_table(mod);

	mod->node_output_bs32_p = buftop();
	(void)zvm_arradd(ZVM_PRG->buf, bs32_n_words(mod->n_node_outputs));

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
		trace_inputs(mod, output_input_dep_bs32, *bufp(mod->outputs_p + i));
		#ifdef VERBOSE_DEBUG
		printf("o[%d]: ", i); bs32_print(mod->n_inputs, output_input_dep_bs32); printf("\n");
		#endif
	}

	// initialize instance->u32 keyval map
	{
		mod->instance_u32_map_p = buftop();

		uint32_t p = mod->code_begin_p;
		const uint32_t p_end = mod->code_end_p;
		mod->n_instance_u32_values = 0;
		while (p < p_end) {
			uint32_t code = *bufp(p);
			int op = code & ZVM_OP_MASK;
			if (op == ZVM_OP(INSTANCE)) {
				uint32_t* xs = zvm_arradd(ZVM_PRG->buf, 2);
				printf("instance p at %d\n", p);
				xs[0] = p;
				xs[1] = 0;
				mod->n_instance_u32_values++;
			}
			p += get_op_length(p);
		}
		zvm_assert(p == p_end);
	}

	return zvm_arrlen(ZVM_PRG->modules) - 1;
}

static uint32_t get_instance_u32_map_index(struct zvm_module* mod, uint32_t instance_p)
{
	uint32_t* pairs = bufp(mod->instance_u32_map_p);
	int left = 0;
	int right = mod->n_instance_u32_values - 1;
	while (left <= right) {
		int mid = (left + right) >> 1;
		uint32_t k = pairs[mid << 1];
		if (k < instance_p) {
			left = mid + 1;
		} else if (k > instance_p) {
			right = mid - 1;
		} else {
			return mid;
		}
	}
	#ifdef VERBOSE_DEBUG
	printf("instance_p=%d not found!\n", instance_p);
	#endif
	zvm_assert(!"instance_p not found");
}

static uint32_t instance_u32_map_get(struct zvm_module* mod, uint32_t instance_p)
{
	uint32_t index = get_instance_u32_map_index(mod, instance_p);
	uint32_t* pairs = bufp(mod->instance_u32_map_p);
	return pairs[(index << 1) + 1];
}

static void instance_u32_map_set(struct zvm_module* mod, uint32_t instance_p, uint32_t value)
{
	uint32_t index = get_instance_u32_map_index(mod, instance_p);
	uint32_t* pairs = bufp(mod->instance_u32_map_p);
	pairs[(index << 1) + 1] = value;
}


static int fnkey_cmp(const void* va, const void* vb)
{
	const struct zvm_fnkey* a = va;
	const struct zvm_fnkey* b = vb;

	int c0 = u32cmp(a->module_id, b->module_id);
	if (c0 != 0) return c0;

	struct zvm_module* mod = &ZVM_PRG->modules[a->module_id];
	const int drain_request_sz = get_module_drain_request_sz(mod);

	if (a->full_drain_request_bs32_p != b->full_drain_request_bs32_p) {
		int c1 = bs32_cmp(drain_request_sz, bufp(a->full_drain_request_bs32_p), bufp(b->full_drain_request_bs32_p));
		if (c1 != 0) return c1;
	}

	if (a->drain_request_bs32_p != b->drain_request_bs32_p) {
		int c2 = bs32_cmp(drain_request_sz, bufp(a->drain_request_bs32_p), bufp(b->drain_request_bs32_p));
		if (c2 != 0) return c2;
	}

	return u32cmp(a->prev_function_id, b->prev_function_id);
}

static inline int drain_request_state_index()
{
	return 0;
}

static inline int drain_request_output_index(int output_index)
{
	return 1+output_index;
}

static inline int drain_request_state_test(uint32_t p)
{
	return bs32_test(bufp(p), drain_request_state_index());
}

static inline int drain_request_output_test(uint32_t p, int output_index)
{
	return bs32_test(bufp(p), drain_request_output_index(output_index));
}

static void drain_request_state_set(uint32_t p)
{
	bs32_set(bufp(p), drain_request_state_index());
}

static void drain_request_output_set(uint32_t p, int output_index)
{
	bs32_set(bufp(p), drain_request_output_index(output_index));
}

#if 0
static int fnkeyval_cmp(const void* va, const void* vb)
{
	const struct zvm_fnkeyval* a = va;
	const struct zvm_fnkeyval* b = vb;
	return fnkey_cmp(&a->key, &b->key);
}
#endif

static int produce_fnkey_function_id(struct zvm_fnkey* key, int* did_insert)
{
	int left = 0;
	int n = zvm_arrlen(ZVM_PRG->fnkeyvals);
	int right = n;
	while (left < right) {
		int mid = (left+right) >> 1;
		if (fnkey_cmp(&ZVM_PRG->fnkeyvals[mid].key, key) < 0) {
			left = mid + 1;
		} else {
			right = mid;
		}
	}

	if (left < n) {
		struct zvm_fnkeyval* val = &ZVM_PRG->fnkeyvals[left];
		if (fnkey_cmp(&val->key, key) == 0) {
			return val->function_id;
		}
	}

	(void)zvm_arradd(ZVM_PRG->fnkeyvals, 1);

	struct zvm_fnkeyval* val = &ZVM_PRG->fnkeyvals[left];

	int to_move = n - left;
	if (to_move > 0) {
		memmove(val+1, val, to_move*sizeof(*val));
	}

	val->key = *key;
	val->function_id = zvm_arrlen(ZVM_PRG->functions);

	struct zvm_function fn = {0};
	fn.key = *key;
	zvm_arrpush(ZVM_PRG->functions, fn);

	if (did_insert) *did_insert = 1;

	return val->function_id;
}


// "DROUT" = "drain or output", which happens to share the same structure...
enum {
	DROUT_P = 0,
	DROUT_INDEX,
	DROUT_COUNTER,
	DROUT_DECR_LIST_N,
	DROUT_DECR_LIST_P,
	DROUT_LEN,
};

#define DROUT_SZ (DROUT_LEN * sizeof(uint32_t))

static int drout_compar(const void* va, const void* vb)
{
	const uint32_t* a = va;
	const uint32_t* b = vb;
	return u32paircmp(a,b);
}

static void push_drout(uint32_t p, int index)
{
	uint32_t* drain = zvm_arradd(ZVM_PRG->buf, DROUT_LEN);
	drain[DROUT_P] = p;
	drain[DROUT_INDEX] = index;
	drain[DROUT_COUNTER] = 0;
	drain[DROUT_DECR_LIST_N] = 0;
	drain[DROUT_DECR_LIST_P] = 0;
}

static int drout_find(uint32_t drouts_p, int n, uint32_t kp, uint32_t ki)
{
	int left = 0;
	int right = n;

	uint32_t k[] = {kp,ki};
	while (left <= right) {
		int mid = (left+right) >> 1;
		int cmp = drout_compar(bufp(drouts_p + mid*DROUT_LEN), k);
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

static void add_drain_instance_output_visitor(struct tracer* tr, uint32_t p, int output_index)
{
	uint32_t code = *bufp(p);
	int module_id = code >> ZVM_OP_BITS;
	zvm_assert(zvm__is_valid_module_id(module_id));
	struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];

	for (int i = 0; i < mod2->n_inputs; i++) {
		if (!bs32_test(get_output_input_dep_bs32(mod2, output_index), i)) {
			continue;
		}
		push_drout(p, i);
	}
}

static inline struct zvm_function* resolve_function_id(uint32_t function_id)
{
	return &ZVM_PRG->functions[function_id];
}

static void add_drain(uint32_t function_id, uint32_t p, int index)
{
	push_drout(p, index);

	struct tracer tr = {
		.mod = &ZVM_PRG->modules[resolve_function_id(function_id)->key.module_id],
		.instance_output_visitor = add_drain_instance_output_visitor
	};
	uint32_t pp = (p == ZVM_NIL_P)
		? bufp(tr.mod->outputs_p)[index]
		: *bufp(zvm__arg_index(p, index));
	trace(&tr, pp);
}

static void drain_to_output_instance_output_visitor_count(struct tracer* tr, uint32_t p, int output_index)
{
	uint32_t* drain = (uint32_t*)tr->usr;
	drain[DROUT_COUNTER]++;

	struct zvm_function* fn = tracer_function(tr);
	int i = drout_find(fn->outputs_p, fn->n_outputs, p, output_index);
	uint32_t* output = bufp(fn->outputs_p + i*DROUT_LEN);
	output[DROUT_DECR_LIST_N]++;
}

static void drain_to_output_instance_output_visitor_write(struct tracer* tr, uint32_t p, int output_index)
{
	struct zvm_function* fn = tracer_function(tr);
	int i = drout_find(fn->outputs_p, fn->n_outputs, p, output_index);
	uint32_t* output = bufp(fn->outputs_p + i*DROUT_LEN);
	bufp(output[DROUT_DECR_LIST_P])[output[DROUT_DECR_LIST_N]++] = *((int*)tr->usr);
}

uint32_t* drout_index_compar_drouts;
static int drout_index_compar(const void* va, const void* vb)
{
	uint32_t a = *(uint32_t*)va;
	uint32_t b = *(uint32_t*)vb;
	return drout_compar(
		&drout_index_compar_drouts[a * DROUT_LEN],
		&drout_index_compar_drouts[b * DROUT_LEN]
	);
}

static int grok_function(uint32_t parent_function_id, uint32_t p, uint32_t output_indices_p, int n_outputs, uint32_t prev_function_id)
{
	uint32_t buftop0 = buftop();

	uint32_t code = *bufp(p);
	int instance_module_id = code >> ZVM_OP_BITS;
	zvm_assert(zvm__is_valid_module_id(instance_module_id));

	struct zvm_module* instance_mod = &ZVM_PRG->modules[instance_module_id];
	const int drain_request_sz = get_module_drain_request_sz(instance_mod);
	struct zvm_function* pfn = &ZVM_PRG->functions[parent_function_id];
	struct zvm_module* parent_mod = &ZVM_PRG->modules[pfn->key.module_id];

	struct zvm_fnkey key = {
		.module_id = instance_module_id,
		.prev_function_id = prev_function_id,
		.drain_request_bs32_p = bs32_alloc(drain_request_sz),
	};

	// populate drain_request_bs32_p
	for (int i = 0; i < n_outputs; i++) {
		uint32_t* output = bufp(pfn->outputs_p + *bufp(output_indices_p + i) * DROUT_LEN);
		zvm_assert(output[DROUT_P] == p);
		uint32_t index = output[DROUT_INDEX];
		if (index == ZVM_NIL_ID) {
			drain_request_state_set(key.drain_request_bs32_p);
		} else {
			drain_request_output_set(key.drain_request_bs32_p, output[DROUT_INDEX]);
		}
	}

	// find full_drain_request_bs32_p; either share with previous function
	// if part of a chain, or generate a new
	if (prev_function_id != ZVM_NIL_ID) {
		struct zvm_fnkey prev_key = ZVM_PRG->functions[prev_function_id].key;
		key.full_drain_request_bs32_p = prev_key.full_drain_request_bs32_p;
		#ifdef DEBUG
		// verify
		uint32_t* node_bs32 = get_node_output_bs32(parent_mod);
		for (int i = 0; i < instance_mod->n_outputs; i++) {
			int node_index = get_node_index(parent_mod, p, i);
			zvm_assert((bs32_test(node_bs32, node_index) == drain_request_output_test(key.full_drain_request_bs32_p, i)) && "unexpected full drain request pattern");
		}
		#endif
	} else {
		// use node visit set to produce full drain request
		key.full_drain_request_bs32_p = bs32_alloc(drain_request_sz);
		uint32_t* node_bs32 = get_node_output_bs32(parent_mod);
		for (int i = 0; i < instance_mod->n_outputs; i++) {
			int node_index = get_node_index(parent_mod, p, i);
			if (bs32_test(node_bs32, node_index)) {
				drain_request_output_set(key.full_drain_request_bs32_p, i);
			}
		}
	}

	int did_insert = 0;
	uint32_t function_id = produce_fnkey_function_id(&key, &did_insert);
	if (!did_insert) {
		// no insert; retract allocations
		zvm_arrsetlen(ZVM_PRG->buf, buftop0);
	}

	return function_id;
}

static void emit_function(uint32_t function_id)
{
	struct zvm_fnkey fnkey = resolve_function_id(function_id)->key;
	struct zvm_module* mod = &ZVM_PRG->modules[fnkey.module_id];

	// calculate future drain request set, which is the full drain request
	// minus the chain of drain requests
	const int drain_request_sz = get_module_drain_request_sz(mod);
	uint32_t future_drain_request_p = bs32_alloc(drain_request_sz);
	bs32_copy(drain_request_sz, bufp(future_drain_request_p), bufp(fnkey.full_drain_request_bs32_p));
	{
		struct zvm_fnkey* fk = &fnkey;
		for (;;) {
			bs32_sub_inplace(drain_request_sz, bufp(future_drain_request_p), bufp(fk->drain_request_bs32_p));
			if (fk->prev_function_id == ZVM_NIL_ID) {
				break;
			}
			fk = &ZVM_PRG->functions[fk->prev_function_id].key;
		}
	}

	clear_node_visit_set(mod);

	// find drains ...
	{
		struct zvm_function* fn = resolve_function_id(function_id);
		fn->n_drains = 0;
		fn->drains_p = buftop();

		for (int output_index = 0; output_index < mod->n_outputs; output_index++) {
			if (!drain_request_output_test(fnkey.drain_request_bs32_p, output_index)) {
				continue;
			}
			add_drain(function_id, ZVM_NIL_P, output_index);
		}

		if (drain_request_state_test(fn->key.drain_request_bs32_p)) {
			uint32_t p = mod->code_begin_p;
			const uint32_t p_end = mod->code_end_p;
			while (p < p_end) {
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;
				if (op == ZVM_OP(UNIT_DELAY)) {
					add_drain(function_id, p, 0);
				} else if (op == ZVM_OP(INSTANCE)) {
					int module_id = code >> ZVM_OP_BITS;
					zvm_assert(zvm__is_valid_module_id(module_id));
					struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];
					int n_inputs = mod2->n_inputs;
					for (int i = 0; i < n_inputs; i++) {
						add_drain(function_id, p, i);
					}
				}
				p += get_op_length(p);
			}
			zvm_assert(p == p_end);
		}

		// sort and compact drain array by removing duplicates

		const int n_drains_with_dupes = (buftop() - fn->drains_p) / DROUT_LEN;
		qsort(
			bufp(fn->drains_p),
			n_drains_with_dupes,
			DROUT_SZ,
			drout_compar);

		uint32_t p_read = fn->drains_p;
		uint32_t p_write = fn->drains_p;
		uint32_t p_end = buftop();

		while (p_read < p_end) {

			if (p_read != p_write) {
				memcpy(bufp(p_write), bufp(p_read), DROUT_SZ);
			}

			uint32_t p0 = p_read;
			do {
				p_read += DROUT_LEN;
			} while (p_read < p_end && drout_compar(bufp(p0), bufp(p_read)) == 0);

			p_write += DROUT_LEN;
		}
		zvm_assert(p_read == p_end);

		fn->n_drains = (p_write - fn->drains_p) / DROUT_LEN;

		zvm_arrsetlen(ZVM_PRG->buf, p_write);
	}

	// find outputs ...
	{
		struct zvm_function* fn = resolve_function_id(function_id);
		fn->n_outputs = 0;
		fn->outputs_p = buftop();

		// as a side effect of finding drains, node_output_bs32_p has
		// 1's for all node outputs visited; for each instance output,
		// add a drout

		uint32_t* node_bs32 = get_node_output_bs32(mod);
		for (int i = 0; i < mod->n_node_outputs; i++) {
			if (!bs32_test(node_bs32, i)) {
				continue;
			}
			uint32_t* node_output = bufp(mod->node_outputs_p + i*2);
			uint32_t p = node_output[0];

			uint32_t code = *bufp(p);
			int op = code & ZVM_OP_MASK;
			if (op != ZVM_OP(INSTANCE)) {
				continue;
			}

			fn->n_outputs++;
			push_drout(p, node_output[1]);
		}
	}

	// initialize counters and decrement lists

	for (int pass = 0; pass < 2; pass++) {
		struct zvm_function* fn = resolve_function_id(function_id);
		const int n_drains = fn->n_drains;
		const int n_outputs = fn->n_outputs;

		if (pass == 1) {
			// reset counters; used for indexing when writing
			// decrement list and are thus reinitialized

			for (int i = 0; i < n_drains; i++) {
				uint32_t* drain = bufp(fn->drains_p + i*DROUT_LEN);
				drain[DROUT_DECR_LIST_N] = 0;
			}

			for (int i = 0; i < n_outputs; i++) {
				uint32_t* output = bufp(fn->outputs_p + i*DROUT_LEN);
				output[DROUT_DECR_LIST_N] = 0;
			}
		}

		for (int i = 0; i < n_drains; i++) {
			uint32_t* drain = bufp(fn->drains_p + i*DROUT_LEN);
			struct tracer tr = {
				.mod = mod,
				.function_id = function_id,
				.break_at_instance = 1, // stop at instance outputs
				.usr = (pass == 0)
					? (void*)drain
					: (void*)&i,
				.instance_output_visitor = (pass == 0)
					? drain_to_output_instance_output_visitor_count
					: drain_to_output_instance_output_visitor_write,
			};
			uint32_t p = (drain[DROUT_P] == ZVM_NIL_P)
				? bufp(tr.mod->outputs_p)[drain[DROUT_INDEX]]
				: *bufp(zvm__arg_index(drain[DROUT_P], drain[DROUT_INDEX]));
			clear_node_visit_set(mod);
			trace(&tr, p);
		}

		for (int i = 0; i < n_outputs; i++) {
			uint32_t* output = bufp(fn->outputs_p + i*DROUT_LEN);
			uint32_t code = *bufp(output[DROUT_P]);
			int op = code & ZVM_OP_MASK;
			zvm_assert(op == ZVM_OP(INSTANCE));
			int module_id = code >> ZVM_OP_BITS;
			zvm_assert(zvm__is_valid_module_id(module_id));
			struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];

			uint32_t* bs32 = get_output_input_dep_bs32(mod2, output[DROUT_INDEX]);
			for (int j = 0; j < mod2->n_inputs; j++) {
				if (!bs32_test(bs32, j)) {
					continue;
				}

				int di = drout_find(fn->drains_p, n_drains, output[DROUT_P], j);
				uint32_t* drain = bufp(fn->drains_p + di*DROUT_LEN);

				if (pass == 0) {
					output[DROUT_COUNTER]++;
					drain[DROUT_DECR_LIST_N]++;
				} else if (pass == 1) {
					bufp(drain[DROUT_DECR_LIST_P])[drain[DROUT_DECR_LIST_N]++] = i;
				} else {
					zvm_assert(!"unreachable");
				}
			}
		}

		if (pass == 0) {
			// allocate decrement lists
			const uint32_t p0 = buftop();
			uint32_t p = p0;

			for (int i = 0; i < n_drains; i++) {
				uint32_t* drain = bufp(fn->drains_p + i*DROUT_LEN);
				drain[DROUT_DECR_LIST_P] = p;
				p += drain[DROUT_DECR_LIST_N];
			}

			for (int i = 0; i < n_outputs; i++) {
				uint32_t* output = bufp(fn->outputs_p + i*DROUT_LEN);
				output[DROUT_DECR_LIST_P] = p;
				p += output[DROUT_DECR_LIST_N];
			}

			(void)zvm_arradd(ZVM_PRG->buf, p - p0);
		}
	}

	{
		struct zvm_function* fn = resolve_function_id(function_id);

		const int n_drains = fn->n_drains;
		uint32_t drain_queue_p = buftop();
		int drain_queue_i = 0;
		int drain_queue_n = 0;
		(void)zvm_arradd(ZVM_PRG->buf, n_drains);

		const int n_outputs = fn->n_outputs;
		uint32_t output_queue_p = buftop();
		int output_queue_i = 0;
		int output_queue_n = 0;
		(void)zvm_arradd(ZVM_PRG->buf, n_outputs);

		{
			uint32_t* drain_queue = bufp(drain_queue_p);
			for (int i = 0; i < n_drains; i++) {
				uint32_t* drain = bufp(fn->drains_p + i*DROUT_LEN);
				if (drain[DROUT_COUNTER] == 0) {
					drain_queue[drain_queue_n++] = i;
				}
			}
		}

		{
			uint32_t* output_queue = bufp(output_queue_p);
			clear_node_visit_set(mod);
			for (int i = 0; i < n_outputs; i++) {
				uint32_t* output = bufp(fn->outputs_p + i*DROUT_LEN);
				if (output[DROUT_COUNTER] == 0) {
					output_queue[output_queue_n++] = i;
				}

				// mark instance output nodes in visit set; this makes
				// it easy to extract the "full drain request" of an
				// instance
				visit_node(mod, output[DROUT_P], output[DROUT_INDEX]);
			}
		}

		// set nil function id
		uint32_t* pairs = bufp(mod->instance_u32_map_p);
		for (int i = 0; i < mod->n_instance_u32_values; i++) {
			pairs[(i << 1) + 1] = ZVM_NIL_ID;
		}

		for (;;) {
			// be careful not to access after push on buf?
			while (drain_queue_i < drain_queue_n) {
				const int drain_index = bufp(drain_queue_p)[drain_queue_i++];
				uint32_t* drain = bufp(fn->drains_p + drain_index*DROUT_LEN);

				#ifdef VERBOSE_DEBUG
				printf("DRAIN i=%d ii=%d ->%d:%d\n", drain_queue_i-1, drain_index, drain[DROUT_P], drain[DROUT_INDEX]);
				#endif

				int n = drain[DROUT_DECR_LIST_N];
				uint32_t p = drain[DROUT_DECR_LIST_P];
				uint32_t* output_queue = bufp(output_queue_p);
				for (int i = 0; i < n; i++) {
					const int output_index = *bufp(p+i);
					uint32_t* output = bufp(fn->outputs_p + output_index*DROUT_LEN);
					zvm_assert(output[DROUT_COUNTER] > 0);
					output[DROUT_COUNTER]--;
					if (output[DROUT_COUNTER] == 0) {
						output_queue[output_queue_n++] = output_index;
					}
				}
			}

			if (output_queue_n > output_queue_i) {
				// sort counter=0 outputs by [p,i]; we prefer
				// to push towards "full function calls"
				// instead of splitting instances into multiple
				// function calls. "full function calls" are
				// easier to detect when ordered by [p,i] (all
				// output requests for a given instance will be
				// sequential)

				{
					uint32_t* output_queue = bufp(output_queue_p);
					drout_index_compar_drouts = bufp(fn->outputs_p);
					qsort(
						&output_queue[output_queue_i],
						output_queue_n - output_queue_i,
						sizeof(*output_queue),
						drout_index_compar);
				}

				int doing_full_calls = 0;
				for (int attempt = 0; attempt < 2; attempt++) {
					const int is_full_call_attempt = (attempt == 0);
					const int is_best_effort_attempt = (attempt == 1);

					if (!is_full_call_attempt && doing_full_calls) {
						break;
					}

					int i = output_queue_i;
					while (i < output_queue_n) {
						uint32_t* output_queue = bufp(output_queue_p);
						int i0 = i;
						uint32_t p0 = bufp(fn->outputs_p + output_queue[i0]*DROUT_LEN)[DROUT_P];
						for (; i < output_queue_n; i++) {
							uint32_t p = bufp(fn->outputs_p + output_queue[i]*DROUT_LEN)[DROUT_P];
							if (p != p0) break;
						}
						int i1 = i;

						uint32_t code = *bufp(p0);
						int op = code & ZVM_OP_MASK;
						zvm_assert(op == ZVM_OP(INSTANCE));
						int instance_module_id = code >> ZVM_OP_BITS;
						zvm_assert(zvm__is_valid_module_id(instance_module_id));
						struct zvm_module* instance_mod = &ZVM_PRG->modules[instance_module_id];

						uint32_t prev_function_id = instance_u32_map_get(mod, p0);

						if (is_full_call_attempt) {
							int n_instance_outputs = instance_mod->n_outputs;
							uint32_t* node_bs32 = get_node_output_bs32(mod);
							i = i0;

							// a call is "full" if the requested instance output set
							// is identical to the sequence of output requests.
							// NOTE: a call can be "full" even if it's part of a
							// function chain (i.e. when an instance is split into
							// multiple functions) as long as it's the last call
							// in the chain
							int is_full_call = 1;
							for (int j = 0; j < n_instance_outputs; j++) {
								int node_index = get_node_index(mod, p0, j);
								if (!bs32_test(node_bs32, node_index)) {
									continue;
								}

								uint32_t* output = bufp(fn->outputs_p + output_queue[i++]*DROUT_LEN);
								if (output[DROUT_INDEX] != j) {
									is_full_call = 0;
									break;
								}
							}

							if (is_full_call) {
								uint32_t instance_function_id = grok_function(
									function_id,
									p0,
									output_queue_p+i0,
									i1-i0,
									prev_function_id);

								// TODO and remove outputs? how?
								doing_full_calls = 1;
							}

							i = i1;
						} else if (is_best_effort_attempt) {
							zvm_assert(!doing_full_calls);
							// TODO ... so uhm.. just count the number of provided outputs...
							// choose the one with the largest number? or should I go for
							// best ratio? or the one that releases the most outputs? :)
							// counting outputs seems easiest, and is probabably good enough?

							//spit_function(prev_function_id);

							// TODO and remove outputs? how?
						} else {
							zvm_assert(!"unreachable");
						}

					}
				}


				#if 0
				for (int i = output_queue_i; i < output_queue_n; i++) {
					uint32_t code = *bufp(p);
					int instance_module_id = code >> ZVM_OP_BITS;
					zvm_assert(zvm__is_valid_module_id(instance_module_id));
					struct zvm_module* instance_mod = &ZVM_PRG->modules[instance_module_id];
				}
				#endif
			}


			break; // XXX
		}

		#if 0
		zvm_assert(drain_queue_i == drain_queue_n);
		zvm_assert(drain_queue_n == n_drains);
		zvm_assert(output_queue_i == output_queue_n);
		zvm_assert(output_queue_n == n_outputs);
		#endif
	}

	#if 0
	#ifdef VERBOSE_DEBUG
	struct zvm_function* fn = resolve_function_id(function_id);
	printf("\n");
	for (int i = 0; i < fn->n_drains; i++) {
		uint32_t* drain = bufp(fn->drains_p + i*DROUT_LEN);
		printf("drain %d: p=%d index=%d counter=%d decr_list_n=%d decr_list_p=%d decr_list=[", i, drain[DROUT_P], drain[DROUT_INDEX], drain[DROUT_COUNTER], drain[DROUT_DECR_LIST_N], drain[DROUT_DECR_LIST_P]);
		for (int j = 0; j < drain[DROUT_DECR_LIST_N]; j++) {
			printf("%s%d", j == 0 ? "" : " ", *bufp(drain[DROUT_DECR_LIST_P]+j));
		}
		printf("]\n");
	}

	printf("\n");
	for (int i = 0; i < fn->n_outputs; i++) {
		struct zvm_function* fn = resolve_function_id(function_id);
		uint32_t* output = bufp(fn->outputs_p + i*DROUT_LEN);
		printf("output %d: p=%d index=%d counter=%d decr_list_n=%d decr_list_p=%d decr_list=[", i, output[DROUT_P], output[DROUT_INDEX], output[DROUT_COUNTER], output[DROUT_DECR_LIST_N], output[DROUT_DECR_LIST_P]);
		for (int j = 0; j < output[DROUT_DECR_LIST_N]; j++) {
			printf("%s%d", j == 0 ? "" : " ", *bufp(output[DROUT_DECR_LIST_P]+j));
		}
		printf("]\n");
	}
	#endif
	#endif

}

void zvm_end_program(uint32_t main_module_id)
{
	ZVM_PRG->main_module_id = main_module_id;
	struct zvm_module* mod = &ZVM_PRG->modules[main_module_id];

	const int drain_request_sz = get_module_drain_request_sz(mod);

	struct zvm_fnkey main_key = {
		.module_id = main_module_id,
		.full_drain_request_bs32_p = bs32_alloc(drain_request_sz),
		.drain_request_bs32_p = bs32_alloc(drain_request_sz),
		.prev_function_id = ZVM_NIL_ID,
	};

	bs32_fill(drain_request_sz, bufp(main_key.full_drain_request_bs32_p), 1);
	bs32_fill(drain_request_sz, bufp(main_key.drain_request_bs32_p), 1);

	emit_function(ZVM_PRG->main_function_id = produce_fnkey_function_id(&main_key, NULL));
}
