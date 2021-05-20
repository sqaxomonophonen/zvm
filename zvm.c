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

static inline int get_module_outcome_request_sz(struct zvm_module* mod)
{
	const int n_state_bits = 1;
	const int n_output_bits = mod->n_outputs;
	return n_state_bits + n_output_bits;
}

static inline int module_has_state(struct zvm_module* mod)
{
	return mod->n_bits > 0;
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

static uint32_t* get_outcome_index_input_dep_bs32(struct zvm_module* mod, uint32_t outcome_index)
{
	if (outcome_index == ZVM_NIL_ID) {
		return get_state_input_dep_bs32(mod);
	} else {
		return get_output_input_dep_bs32(mod, outcome_index);
	}
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
	uint32_t* node_bs32 = bufp(mod->node_output_bs32_p);
	if (bs32_test(node_bs32, node_index)) {
		return 0;
	} else {
		bs32_set(node_bs32, node_index);
		return 1;
	}
}

struct tracer {
	struct zvm_module* mod;
	uint32_t substance_id;

	void(*module_input_visitor)(struct tracer*, uint32_t p);
	void(*instance_output_visitor)(struct tracer*, uint32_t p, int output_index);

	int break_at_instance;

	void* usr;
};

static inline struct zvm_substance* tracer_substance(struct tracer* tr)
{
	return &ZVM_PRG->substances[tr->substance_id];
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

static uint32_t module_alloc_node_output_bs32(struct zvm_module* mod)
{
	uint32_t top = buftop();
	(void)zvm_arradd(ZVM_PRG->buf, bs32_n_words(mod->n_node_outputs));
	return top;
}

int zvm_end_module()
{
	struct zvm_module* mod = ZVM_MOD;

	mod->code_end_p = buftop();

	build_node_table(mod);

	mod->node_output_bs32_p = module_alloc_node_output_bs32(mod);

	// initialize input bitsets
	{
		const int n_words = mod_n_input_bs32_words(mod);
		mod->input_bs32s_p = buftop();
		const int n_input_bs32s = 1 + mod->n_inputs;
		uint32_t* input_bs32s = zvm_arradd(ZVM_PRG->buf, n_input_bs32s * n_words);
		memset(input_bs32s, 0, sizeof(*input_bs32s) * n_input_bs32s * n_words);
	}

	const int module_id = zvm_arrlen(ZVM_PRG->modules) - 1;

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
		trace_inputs(mod, output_input_dep_bs32, *bufp(mod->outputs_p + i));
		#ifdef VERBOSE_DEBUG
		printf("o[%d]: ", i); bs32_print(mod->n_inputs, output_input_dep_bs32); printf("\n");
		#endif
	}
	#ifdef VERBOSE_DEBUG
	printf("\n");
	#endif

	return module_id;
}

static int substance_key_cmp(const void* va, const void* vb)
{
	const struct zvm_substance_key* a = va;
	const struct zvm_substance_key* b = vb;

	int c0 = u32cmp(a->module_id, b->module_id);
	if (c0 != 0) return c0;

	struct zvm_module* mod = &ZVM_PRG->modules[a->module_id];
	const int outcome_request_sz = get_module_outcome_request_sz(mod);

	if (a->outcome_request_bs32_p != b->outcome_request_bs32_p) {
		return bs32_cmp(outcome_request_sz, bufp(a->outcome_request_bs32_p), bufp(b->outcome_request_bs32_p));
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

static inline int outcome_request_state_test(uint32_t p)
{
	return bs32_test(bufp(p), outcome_request_state_index());
}

static inline int outcome_request_output_test(uint32_t p, int output_index)
{
	return bs32_test(bufp(p), outcome_request_output_index(output_index));
}

static void outcome_request_state_set(uint32_t p)
{
	bs32_set(bufp(p), outcome_request_state_index());
}

static void outcome_request_output_set(uint32_t p, int output_index)
{
	bs32_set(bufp(p), outcome_request_output_index(output_index));
}

static int produce_substance_id_for_key(struct zvm_substance_key* key, int* did_insert)
{
	// leftmost binary search; finds either an existing key (in which case,
	// don't insert), or the proper insertion index
	int left = 0;
	int n = zvm_arrlen(ZVM_PRG->substance_keyvals);
	int right = n;
	while (left < right) {
		int mid = (left+right) >> 1;
		if (substance_key_cmp(&ZVM_PRG->substance_keyvals[mid].key, key) < 0) {
			left = mid + 1;
		} else {
			right = mid;
		}
	}

	if (left < n) {
		struct zvm_substance_keyval* keyval = &ZVM_PRG->substance_keyvals[left];
		if (substance_key_cmp(&keyval->key, key) == 0) {
			return keyval->substance_id;
		}
	}

	// grow array by one
	(void)zvm_arradd(ZVM_PRG->substance_keyvals, 1);

	struct zvm_substance_keyval* keyval = &ZVM_PRG->substance_keyvals[left];

	int to_move = n - left;
	if (to_move > 0) {
		memmove(keyval+1, keyval, to_move*sizeof(*keyval));
	}

	keyval->key = *key;
	keyval->substance_id = zvm_arrlen(ZVM_PRG->substances);

	struct zvm_substance sb = {0};
	sb.key = *key;
	#if 0
	fn_init_instance_u32_map(&fn);
	#endif
	zvm_arrpush(ZVM_PRG->substances, sb);

	if (did_insert) *did_insert = 1;

	return keyval->substance_id;
}


// "DROUT" = "drain or outcome", which happens to share the same structure...
enum {
	DROUT_P = 0,
	DROUT_INDEX,
	DROUT_COUNTER,
	DROUT_DECR_LIST_N,
	DROUT_DECR_LIST_P,
	DROUT_USR,
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
	drain[DROUT_USR] = 0;
}

static int drout_find_index(uint32_t drouts_p, int n, uint32_t kp, uint32_t ki)
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

static uint32_t* drout_find(uint32_t drouts_p, int n, uint32_t kp, uint32_t ki)
{
	return bufp(drouts_p + drout_find_index(drouts_p, n, kp, ki)*DROUT_LEN);
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

static inline struct zvm_substance* resolve_substance_id(uint32_t substance_id)
{
	return &ZVM_PRG->substances[substance_id];
}

static void add_drain(uint32_t substance_id, uint32_t p, int index)
{
	push_drout(p, index);

	struct tracer tr = {
		.mod = &ZVM_PRG->modules[resolve_substance_id(substance_id)->key.module_id],
		.instance_output_visitor = add_drain_instance_output_visitor
	};
	uint32_t pp = (p == ZVM_NIL_P)
		? bufp(tr.mod->outputs_p)[index]
		: *bufp(zvm__arg_index(p, index));
	trace(&tr, pp);
}

struct drain_to_output_instance_output_visitor_usr {
	uint32_t* drain;
	uint32_t setp;
	uint32_t n_outcomes;
	uint32_t outcomes_p;
};

static void drain_to_output_instance_output_visitor_count(struct tracer* tr, uint32_t p, int output_index)
{
	struct drain_to_output_instance_output_visitor_usr* usr = tr->usr;
	usr->drain[DROUT_COUNTER]++;
	uint32_t* outcome = drout_find(usr->outcomes_p, usr->n_outcomes, p, output_index);
	outcome[DROUT_DECR_LIST_N]++;
}

static void drain_to_output_instance_output_visitor_write(struct tracer* tr, uint32_t p, int output_index)
{
	struct drain_to_output_instance_output_visitor_usr* usr = tr->usr;
	uint32_t* outcome = drout_find(usr->outcomes_p, usr->n_outcomes, p, output_index);
	bufp(outcome[DROUT_DECR_LIST_P])[outcome[DROUT_DECR_LIST_N]++] = usr->setp;
}

#define ENCODE_DRAIN(v)    ((v)&0x7fffffff)
#define ENCODE_OUTCOME(v)  ((v)|0x80000000)
#define GET_VALUE(v)       ((v)&0x7fffffff)
#define IS_DRAIN(v)        (((v)&0x80000000)==0)
#define IS_OUTCOME(v)      !IS_DRAIN(v)

static int drain_outcome_compar(const void* va, const void* vb)
{
	uint32_t a = *(uint32_t*)va;
	uint32_t b = *(uint32_t*)vb;
	int c0 = IS_OUTCOME(a) - IS_OUTCOME(b);
	if (c0 != 0) return c0;
	return a-b;
}


uint32_t* compar_drouts;
static int queue_outcome_pi_compar(const void* va, const void* vb)
{
	uint32_t qa = *(uint32_t*)va;
	uint32_t qb = *(uint32_t*)vb;
	zvm_assert(IS_OUTCOME(qa));
	zvm_assert(IS_OUTCOME(qb));
	uint32_t a = GET_VALUE(qa);
	uint32_t b = GET_VALUE(qb);

	return drout_compar(
		&compar_drouts[a*DROUT_LEN],
		&compar_drouts[b*DROUT_LEN]
	);
}

static int queue_outcome_full_compar(const void* va, const void* vb)
{
	uint32_t qa = *(uint32_t*)va;
	uint32_t qb = *(uint32_t*)vb;
	zvm_assert(IS_OUTCOME(qa));
	zvm_assert(IS_OUTCOME(qb));
	uint32_t a = GET_VALUE(qa);
	uint32_t b = GET_VALUE(qb);
	uint32_t* oa = &compar_drouts[a*DROUT_LEN];
	uint32_t* ob = &compar_drouts[b*DROUT_LEN];
	int c0 = ob[DROUT_USR] - oa[DROUT_USR];
	if (c0 != 0) return c0;
	return drout_compar(oa, ob);
}

static int queue_outcome_get_pspan_length(uint32_t outcomes_p, uint32_t* queue, int n, int break_on_drain)
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
		uint32_t* outcome = bufp(outcomes_p + GET_VALUE(qv)*DROUT_LEN);
		uint32_t p = outcome[DROUT_P];
		if (p0 == ZVM_NIL_P) {
			p0 = p;
		} else if (p != p0) {
			return i;
		}
	}
	return n;
}

static void substance_sequence_push(uint32_t p0, uint32_t ack_substance_id)
{
	uint32_t* xs = zvm_arradd(ZVM_PRG->buf, 2);
	xs[0] = p0;
	xs[1] = ack_substance_id;
}

static int ack_substance(uint32_t p, uint32_t queue_i, int n, uint32_t queue_p, int* queue_np, uint32_t drains_p, uint32_t outcomes_p, uint32_t** update_queue, int lookup_only)
{
	uint32_t buftop0 = buftop();

	uint32_t code = *bufp(p);
	int instance_module_id = code >> ZVM_OP_BITS;
	zvm_assert(zvm__is_valid_module_id(instance_module_id));

	struct zvm_module* instance_mod = &ZVM_PRG->modules[instance_module_id];
	const int outcome_request_sz = get_module_outcome_request_sz(instance_mod);

	struct zvm_substance_key key = {
		.module_id = instance_module_id,
		.outcome_request_bs32_p = bs32_alloc(outcome_request_sz),
	};

	uint32_t* queue = bufp(queue_p + queue_i);
	uint32_t* drains = bufp(drains_p);
	uint32_t* outcomes = bufp(outcomes_p);
	for (int i = 0; i < n; i++) {
		// populate outcome_request_bs32_p ...
		uint32_t qv = queue[i];
		zvm_assert(IS_OUTCOME(qv));
		uint32_t outcome_index = GET_VALUE(qv);
		uint32_t* outcome = &outcomes[outcome_index * DROUT_LEN];
		zvm_assert(outcome[DROUT_P] == p);
		uint32_t index = outcome[DROUT_INDEX];
		if (index == ZVM_NIL_ID) {
			outcome_request_state_set(key.outcome_request_bs32_p);
		} else {
			outcome_request_output_set(key.outcome_request_bs32_p, outcome[DROUT_INDEX]);
		}

		if (!lookup_only) {
			// potentially release outcomes ...
			uint32_t decr_list_n = outcome[DROUT_DECR_LIST_N];
			uint32_t decr_list_p = outcome[DROUT_DECR_LIST_P];
			for (int i = 0; i < decr_list_n; i++) {
				uint32_t drain_index = *bufp(decr_list_p + i);
				uint32_t* drain = &drains[drain_index * DROUT_LEN];
				zvm_assert(drain[DROUT_COUNTER] > 0 && "decrement when zero not expected");
				drain[DROUT_COUNTER]--;
				if (drain[DROUT_COUNTER] == 0) {
					*bufp(queue_p + ((*queue_np)++)) = ENCODE_DRAIN(drain_index);
				}
			}
		}
	}

	int did_insert = 0;
	uint32_t produced_substance_id = produce_substance_id_for_key(&key, &did_insert);

	zvm_assert((lookup_only == 0 || did_insert == 0) && "not expecting insert for lookup-only calls");

	if (!did_insert) {
		// no insert; rollback allocations
		zvm_arrsetlen(ZVM_PRG->buf, buftop0);
	}

	// keep pointers valid...
	*update_queue = bufp(queue_p);

	return produced_substance_id;
}

static void process_substance(uint32_t substance_id)
{
	struct zvm_substance_key key = resolve_substance_id(substance_id)->key;
	struct zvm_module* mod = &ZVM_PRG->modules[key.module_id];

	clear_node_visit_set(mod);

	int n_drains = 0;
	int n_outcomes = 0;
	uint32_t drains_p;
	uint32_t outcomes_p;

	// find drains ...
	{
		drains_p = buftop();

		for (int output_index = 0; output_index < mod->n_outputs; output_index++) {
			if (!outcome_request_output_test(key.outcome_request_bs32_p, output_index)) {
				continue;
			}
			add_drain(substance_id, ZVM_NIL_P, output_index);
		}

		if (outcome_request_state_test(key.outcome_request_bs32_p)) {
			uint32_t p = mod->code_begin_p;
			const uint32_t p_end = mod->code_end_p;
			while (p < p_end) {
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;
				if (op == ZVM_OP(UNIT_DELAY)) {
					add_drain(substance_id, p, 0);
				} else if (op == ZVM_OP(INSTANCE)) {
					int module_id = code >> ZVM_OP_BITS;
					zvm_assert(zvm__is_valid_module_id(module_id));
					struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];
					if (module_has_state(mod2)) {
						int n_inputs = mod2->n_inputs;
						uint32_t* ibs = get_state_input_dep_bs32(mod2);
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

		const int n_drains_with_dupes = (buftop() - drains_p) / DROUT_LEN;
		qsort(
			bufp(drains_p),
			n_drains_with_dupes,
			DROUT_SZ,
			drout_compar);

		uint32_t read_p = drains_p;
		uint32_t write_p = drains_p;
		uint32_t p_end = buftop();

		while (read_p < p_end) {

			if (read_p != write_p) {
				memcpy(bufp(write_p), bufp(read_p), DROUT_SZ);
			}

			uint32_t p0 = read_p;
			do {
				read_p += DROUT_LEN;
			} while (read_p < p_end && drout_compar(bufp(p0), bufp(read_p)) == 0);

			write_p += DROUT_LEN;
		}
		zvm_assert(read_p == p_end);

		n_drains = (write_p - drains_p) / DROUT_LEN;

		zvm_arrsetlen(ZVM_PRG->buf, write_p);
	}

	// find outcomes ...
	{
		outcomes_p = buftop();

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

			n_outcomes++;
			push_drout(p, node_output[1]);
		}

		if (outcome_request_state_test(key.outcome_request_bs32_p)) {
			// add state outcome requests
			uint32_t p = mod->code_begin_p;
			const uint32_t p_end = mod->code_end_p;
			while (p < p_end) {
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;
				if (op == ZVM_OP(INSTANCE)) {
					int module_id = code >> ZVM_OP_BITS;
					zvm_assert(zvm__is_valid_module_id(module_id));
					struct zvm_module* instance_mod = &ZVM_PRG->modules[module_id];
					if (module_has_state(instance_mod)) {
						n_outcomes++;
						push_drout(p, ZVM_NIL_ID);
					}
				}
				p += get_op_length(p);
			}
			zvm_assert(p == p_end);

			// NOTE: assumption here that drouts only require
			// sorting when request_state is true
			qsort(
				bufp(outcomes_p),
				n_outcomes,
				DROUT_SZ,
				drout_compar);
		}
	}

	// initialize counters and decrement lists

	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			// reset counters; used for indexing when writing
			// decrement list and are thus reinitialized

			for (int i = 0; i < n_drains; i++) {
				uint32_t* drain = bufp(drains_p + i*DROUT_LEN);
				drain[DROUT_DECR_LIST_N] = 0;
			}

			for (int i = 0; i < n_outcomes; i++) {
				uint32_t* outcome = bufp(outcomes_p + i*DROUT_LEN);
				outcome[DROUT_DECR_LIST_N] = 0;
			}
		}

		for (int i = 0; i < n_drains; i++) {
			uint32_t* drain = bufp(drains_p + i*DROUT_LEN);
			struct drain_to_output_instance_output_visitor_usr usr = {
				.n_outcomes = n_outcomes,
				.outcomes_p = outcomes_p,
			};
			struct tracer tr = {
				.mod = mod,
				.substance_id = substance_id,
				.break_at_instance = 1, // stop at instance outputs
				.usr = &usr,
			};
			if (pass == 0) {
				tr.instance_output_visitor = drain_to_output_instance_output_visitor_count;
				usr.drain = drain;
			} else {
				tr.instance_output_visitor = drain_to_output_instance_output_visitor_write;
				usr.setp = i;
			}
			uint32_t p = (drain[DROUT_P] == ZVM_NIL_P)
				? bufp(tr.mod->outputs_p)[drain[DROUT_INDEX]]
				: *bufp(zvm__arg_index(drain[DROUT_P], drain[DROUT_INDEX]));
			clear_node_visit_set(mod);
			trace(&tr, p);
		}

		for (int i = 0; i < n_outcomes; i++) {
			uint32_t* outcome = bufp(outcomes_p + i*DROUT_LEN);
			uint32_t code = *bufp(outcome[DROUT_P]);
			int op = code & ZVM_OP_MASK;
			zvm_assert(op == ZVM_OP(INSTANCE));
			int module_id = code >> ZVM_OP_BITS;
			zvm_assert(zvm__is_valid_module_id(module_id));
			struct zvm_module* mod2 = &ZVM_PRG->modules[module_id];

			uint32_t* bs32 = get_outcome_index_input_dep_bs32(mod2, outcome[DROUT_INDEX]);
			for (int j = 0; j < mod2->n_inputs; j++) {
				if (!bs32_test(bs32, j)) {
					continue;
				}

				uint32_t* drain = drout_find(drains_p, n_drains, outcome[DROUT_P], j);

				if (pass == 0) {
					outcome[DROUT_COUNTER]++;
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
				uint32_t* drain = bufp(drains_p + i*DROUT_LEN);
				drain[DROUT_DECR_LIST_P] = p;
				p += drain[DROUT_DECR_LIST_N];
			}

			for (int i = 0; i < n_outcomes; i++) {
				uint32_t* outcome = bufp(outcomes_p + i*DROUT_LEN);
				outcome[DROUT_DECR_LIST_P] = p;
				p += outcome[DROUT_DECR_LIST_N];
			}

			(void)zvm_arradd(ZVM_PRG->buf, p - p0);
		}
	}

	const int new_substance_ids_begin = zvm_arrlen(ZVM_PRG->substances);

	// initialize drain/outcome queue
	const int queue_sz = n_drains+n_outcomes;
	uint32_t queue_p = buftop();
	(void)zvm_arradd(ZVM_PRG->buf, queue_sz);


	// NOTE new values are pushed onto the underlying arrays, so be
	// careful to keep these pointers fresh
	uint32_t* queue = bufp(queue_p);

	int queue_i = 0;
	int queue_n = 0;

	for (int i = 0; i < n_drains; i++) {
		uint32_t* drain = bufp(drains_p + i*DROUT_LEN);
		if (drain[DROUT_COUNTER] == 0) {
			queue[queue_n++] = ENCODE_DRAIN(i);
		}

	}

	clear_node_visit_set(mod);

	for (int i = 0; i < n_outcomes; i++) {
		uint32_t* outcome = bufp(outcomes_p + i*DROUT_LEN);
		if (outcome[DROUT_COUNTER] == 0) {
			queue[queue_n++] = ENCODE_OUTCOME(i);
		}

		uint32_t index = outcome[DROUT_INDEX];
		if (index != ZVM_NIL_ID) {
			// mark instance outcome nodes in visit set;
			// used to detect if a sequence of instance
			// output outcomes constitute a "non-fragmented
			// call"
			visit_node(mod, outcome[DROUT_P], index);
		}
	}

	while (queue_i < queue_n) {
		int has_drains = 0;
		int has_outcomes = 0;

		for (int i = queue_i; i < queue_n; i++) {
			uint32_t qv = queue[i];
			if (IS_DRAIN(qv)) {
				has_drains = 1;
				break;
			}
			if (IS_OUTCOME(qv)) {
				has_outcomes = 1;
			}
		}

		if (has_drains) {
			// execute drains as long as they're available

			qsort(&queue[queue_i], queue_n-queue_i, sizeof *queue, drain_outcome_compar);

			for (; queue_i < queue_n; queue_i++) {
				uint32_t qv = queue[queue_i];

				if (IS_OUTCOME(qv)) break;

				const int drain_index = GET_VALUE(qv);
				uint32_t* drain = bufp(drains_p + drain_index*DROUT_LEN);

				int n = drain[DROUT_DECR_LIST_N];
				uint32_t p = drain[DROUT_DECR_LIST_P];
				for (int i = 0; i < n; i++) {
					const int outcome_index = *bufp(p+i);
					uint32_t* outcome = bufp(outcomes_p + outcome_index*DROUT_LEN);
					zvm_assert(outcome[DROUT_COUNTER] > 0);
					outcome[DROUT_COUNTER]--;
					if (outcome[DROUT_COUNTER] == 0) {
						queue[queue_n++] = ENCODE_OUTCOME(outcome_index);
					}
				}
			}
			continue;
		}

		zvm_assert(!has_drains && has_outcomes);

		// now there are only outcomes in the [queue_i;queue_n]
		// interval.

		compar_drouts = bufp(outcomes_p);
		qsort(&queue[queue_i], queue_n-queue_i, sizeof *queue, queue_outcome_pi_compar);

		#if 0
		for (int i = queue_i; i < queue_n; i++) zvm_assert(IS_OUTCOME(queue[i]));
		#endif

		// look for full calls ...
		int n_full_calls = 0;
		for (int i = queue_i; i < queue_n; ) {
			int pspan_length = queue_outcome_get_pspan_length(outcomes_p, &queue[i], queue_n-i, 0);

			uint32_t p0 = bufp(outcomes_p + GET_VALUE(queue[i])*DROUT_LEN)[DROUT_P];
			uint32_t code = *bufp(p0);
			int op = code & ZVM_OP_MASK;
			zvm_assert(op == ZVM_OP(INSTANCE));
			int instance_module_id = code >> ZVM_OP_BITS;
			zvm_assert(zvm__is_valid_module_id(instance_module_id));
			struct zvm_module* instance_mod = &ZVM_PRG->modules[instance_module_id];

			const int n_instance_outputs = instance_mod->n_outputs;
			uint32_t* node_bs32 = get_node_output_bs32(mod);

			// a call is "full" if the inferred outcome
			// request is identical to the remaining
			// outcome request
			int is_full_call = 1;
			int ii = 0;
			int requesting_state = module_has_state(instance_mod) && outcome_request_state_test(key.outcome_request_bs32_p);
			int n_checks = n_instance_outputs + (requesting_state ? 1 : 0);
			for (int j = 0; j < n_checks; j++) {
				const int is_output = (j < n_instance_outputs);
				const int is_state  = (j == n_instance_outputs);

				uint32_t expected_drout_index = 0;
				if (is_output) {
					int node_index = get_node_index(mod, p0, j);
					if (!bs32_test(node_bs32, node_index)) {
						continue;
					}
					expected_drout_index = j;
				} else if (is_state) {
					expected_drout_index = ZVM_NIL_ID;
				} else {
					zvm_assert(!"unreachable");
				}

				uint32_t* outcome = bufp(outcomes_p + GET_VALUE(queue[i + (ii++)])*DROUT_LEN);
				zvm_assert(outcome[DROUT_P] == p0);
				if (outcome[DROUT_INDEX] != expected_drout_index) {
					is_full_call = 0;
					break;
				}
			}

			if (is_full_call) {
				zvm_assert(ii == pspan_length);
			}


			if (is_full_call) n_full_calls++;

			for (int j = 0; j < pspan_length; j++) {
				uint32_t* outcome = bufp(outcomes_p + GET_VALUE(queue[i+j])*DROUT_LEN);
				outcome[DROUT_USR] = is_full_call;
			}

			i += pspan_length;
		}

		if (n_full_calls > 0) {
			{
				// place full calls in beginning of queue
				compar_drouts = bufp(outcomes_p);
				qsort(&queue[queue_i], queue_n-queue_i, sizeof *queue, queue_outcome_full_compar);
			}

			int queue_n0 = queue_n;
			while (n_full_calls > 0 && queue_i < queue_n0) {
				int pspan_length = queue_outcome_get_pspan_length(outcomes_p, &queue[queue_i], queue_n0-queue_i, 0);

				uint32_t p0 = bufp(outcomes_p + GET_VALUE(queue[queue_i])*DROUT_LEN)[DROUT_P];

				ack_substance(
					p0,
					queue_i,
					pspan_length,
					queue_p,
					&queue_n,
					drains_p,
					outcomes_p,
					&queue,
					0);

				queue_i += pspan_length;
				n_full_calls--;
			}

			zvm_assert((n_full_calls == 0) && "expected to handle all full calls");

		} else {
			// no full calls to choose from, so pick a
			// partial call. it seems complicated to figure
			// out which partial call is the best pick,
			// complicated as in O(n!) maybe? however,
			// reaching this point indicates that instance
			// splitting is inevitable.

			// current attempt at a heuristic solution is
			// to pick the first partial call and continue
			// :)

			int pspan_length = queue_outcome_get_pspan_length(outcomes_p, &queue[queue_i], queue_n-queue_i, 0);
			uint32_t p0 = bufp(outcomes_p + GET_VALUE(queue[queue_i])*DROUT_LEN)[DROUT_P];

			ack_substance(
				p0,
				queue_i,
				pspan_length,
				queue_p,
				&queue_n,
				drains_p,
				outcomes_p,
				&queue,
				0);

			queue_i += pspan_length;
		}
	}

	zvm_assert(queue_i == queue_sz);
	zvm_assert(queue_n == queue_sz);

	// go through queue again; this time construct "the sequence"; I'd
	// prefer constructing in the loop above, but ack_substance() *also*
	// mutates buf

	struct zvm_substance* sb = resolve_substance_id(substance_id);
	sb->sequence_p = buftop();
	queue_i = 0;
	while (queue_i < queue_n) {
		zvm_assert(queue == bufp(queue_p) && "sanity check failed; queue ptr was NOT kept fresh");

		uint32_t qv = queue[queue_i];
		if (IS_DRAIN(qv)) {
			uint32_t v = GET_VALUE(qv);
			uint32_t* drain = bufp(drains_p + v*DROUT_LEN);
			uint32_t p = drain[DROUT_P];
			if (p != ZVM_NIL_P) {
				zvm_assert(!ZVM_IS_SPECIAL(p));
				uint32_t code = *bufp(p);
				int op = code & ZVM_OP_MASK;
				if (op == ZVM_OP(UNIT_DELAY)) {
					substance_sequence_push(p, ZVM_NIL_ID);
				}
			}
			queue_i++;
			continue;
		}

		int pspan_length = queue_outcome_get_pspan_length(outcomes_p, &queue[queue_i], queue_n-queue_i, 1);
		zvm_assert(pspan_length > 0);

		uint32_t p0 = bufp(outcomes_p + GET_VALUE(queue[queue_i])*DROUT_LEN)[DROUT_P];

		const uint32_t top0 = buftop();
		const int n_substances0 = zvm_arrlen(ZVM_PRG->substances);

		uint32_t ack_substance_id = ack_substance(
			p0,
			queue_i,
			pspan_length,
			queue_p,
			&queue_n,
			drains_p,
			outcomes_p,
			&queue,
			1);

		zvm_assert((buftop() == top0) && (zvm_arrlen(ZVM_PRG->substances) == n_substances0) && "expected ack_substance() to FIND, not INSERT, substance; value mutation indicates this is not the case");

		substance_sequence_push(p0, ack_substance_id);

		queue_i += pspan_length;
	}
	sb->sequence_len = (buftop() - sb->sequence_p) >> 1;

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

	const int new_substance_ids_end = zvm_arrlen(ZVM_PRG->substances);
	for (int new_substance_id = new_substance_ids_begin; new_substance_id < new_substance_ids_end; new_substance_id++) {
		process_substance(new_substance_id);
	}
}

static void transmogrify_substance_rec(int substance_id)
{
	struct zvm_substance* sb = &ZVM_PRG->substances[substance_id];

	sb->refcount++;

	if (sb->transmogrified) return;
	sb->transmogrified = 1;

	int sequence_len = sb->sequence_len;
	for (int i = 0; i < sequence_len; i++) {
		uint32_t* xs = bufp(sb->sequence_p + (i<<1));
		//uint32_t p = xs[0];
		uint32_t substance_id = xs[1];
		if (substance_id == ZVM_NIL_ID) {
			continue;
		}
		transmogrify_substance_rec(substance_id);
	}
}

static void transmogrify_substance(int root_substance_id)
{
	transmogrify_substance_rec(root_substance_id);

	#ifdef VERBOSE_DEBUG
	const int n_substances = zvm_arrlen(ZVM_PRG->substances);
	for (int i = 0; i < n_substances; i++) {
		struct zvm_substance* sb = &ZVM_PRG->substances[i];
		const int has_state = outcome_request_state_test(sb->key.outcome_request_bs32_p);
		printf("transmogrif; substance=%d; module=%d; state=%d; refcount=%d\n", i, sb->key.module_id, has_state, sb->refcount);
	}
	printf("\n");
	#endif
}

void zvm_end_program(uint32_t main_module_id)
{
	int buf_sz_after_end_program = buftop();

	ZVM_PRG->main_module_id = main_module_id;
	struct zvm_module* mod = &ZVM_PRG->modules[main_module_id];

	const int outcome_request_sz = get_module_outcome_request_sz(mod);

	struct zvm_substance_key main_key = {
		.module_id = main_module_id,
		.outcome_request_bs32_p = bs32_alloc(outcome_request_sz),
	};

	bs32_fill(outcome_request_sz, bufp(main_key.outcome_request_bs32_p), 1);

	int did_insert = 0;
	process_substance(ZVM_PRG->main_substance_id = produce_substance_id_for_key(&main_key, &did_insert));
	zvm_assert(did_insert);

	int buf_sz_after_process_substance = buftop();

	transmogrify_substance(ZVM_PRG->main_substance_id);

	#ifdef VERBOSE_DEBUG
	printf("=======================================\n");
	const int n_substances = zvm_arrlen(ZVM_PRG->substances);
	printf("n_substances: %d\n", n_substances);
	for (int i = 0; i < n_substances; i++) {
		struct zvm_substance* sb = &ZVM_PRG->substances[i];
		struct zvm_substance_key* key = &sb->key;
		printf("   SB[%d] :: module_id=%d", i , key->module_id);

		const int outcome_request_sz = get_module_outcome_request_sz(&ZVM_PRG->modules[key->module_id]);

		printf(" rq=");
		bs32_print(outcome_request_sz, bufp(key->outcome_request_bs32_p));

		printf(" seqlen=%d", sb->sequence_len);

		printf("\n");
	}
	printf("buf sz after end program:        %d\n", buf_sz_after_end_program);
	printf("buf sz after process substance:  %d\n", buf_sz_after_process_substance);
	printf("=======================================\n");
	#endif
}
