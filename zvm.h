#ifndef ZVM_H

#include <stdint.h>
#include <assert.h>

#define ZVM_OPS \
	\
	ZOP(NOR,2) \
	ZOP(UNIT_DELAY,1) \
	ZOP(INSTANCE,-1) \
	ZOP(UNPACK,1)


#define ZVM_OP(op) ZVM_OP_##op


#define ZVM_SPECIAL0           (0xf0000000)
#define ZVM_SPECIALXY(x,y)     (ZVM_SPECIAL0 + (((x)&0xfff)<<16) + ((y)&0xffff))
#define ZVM_IS_SPECIALX(v,x)   (((v)&0xffff0000) == ZVM_SPECIALXY(x,0))
#define ZVM_IS_SPECIAL(v)      ((v) >= ZVM_SPECIAL0)
#define ZVM_GET_SPECIALY(v)    ((v) & 0xffff)

#define ZVM_X_TAG   (1)
#define ZVM_X_CONST (2)
#define ZVM_X_INPUT (3)

#define ZVM_PLACEHOLDER      ZVM_SPECIALXY(ZVM_X_TAG,   1)
#define ZVM_ZERO             ZVM_SPECIALXY(ZVM_X_CONST, 0)
#define ZVM_ONE              ZVM_SPECIALXY(ZVM_X_CONST, 1)
#define ZVM_INPUT(i)         ZVM_SPECIALXY(ZVM_X_INPUT, i)


#define ZVM_NIL_ID ((uint32_t)-1)
#define ZVM_NIL_P  ((uint32_t)-1)


#define ZVM_OP_BITS (8)
#define ZVM_OP_MASK ((1<<ZVM_OP_BITS )-1)
#define ZVM_MAX_OP_ARG (1<<(32-ZVM_OP_BITS))

#define ZVM_ARRAY_LENGTH(x) (sizeof(x)/sizeof(x[0]))

#define zvm_assert assert

enum zvm_ops {
	ZVM_OP_NIL = 0,
	#define ZOP(op,narg) ZVM_OP(op),
	ZVM_OPS
	#undef ZOP
	ZVM_OP_N
};

// stolen from nothings/stb/stretchy_buffer.h
void* zvm__grow_impl(void* xs, int increment, int item_sz);
#define zvm__magic(a)        ((int *) (void *) (a) - 2)
#define zvm__cap(a)          zvm__magic(a)[0]
#define zvm__len(a)          zvm__magic(a)[1]
#define zvm__needgrow(a,n)   ((a)==0 || zvm__len(a)+(n) >= zvm__cap(a))
#define zvm__grow(a,n)       (*((void **)&(a)) = zvm__grow_impl((a), (n), sizeof(*(a))))
#define zvm__maybegrow(a,n)  (zvm__needgrow(a,(n)) ? zvm__grow(a,n) : 0)
#define zvm_arrpush(a,v)     (zvm__maybegrow(a,1), (a)[zvm__len(a)++] = (v))
#define zvm_arrlen(a)        ((a) ? zvm__len(a) : 0)
#define zvm_arradd(a,n)      (zvm__maybegrow(a,n), zvm__len(a)+=(n), &(a)[zvm__len(a)-(n)])
#define zvm_arrsetlen(a,n)   (((n)>zvm__len(a)) ? (void)zvm__maybegrow(a,((n)-zvm__len(a))) : (void)(zvm__len(a)=(n)))


struct zvm_module {
	int n_inputs;
	int n_outputs;
	// TODO I/O type signature? they're all bits to begin with, until
	// they're not, e.g. for platform semi-independent optimizations like
	// "integer SIMD", or floating point stuff

	uint32_t outputs_p;
	uint32_t code_begin_p;
	uint32_t code_end_p;

	int n_node_outputs;
	uint32_t node_outputs_p;
	uint32_t node_output_bs32_p;

	uint32_t input_bs32s_p;

	int n_bits;
	int refcount;
};

struct zvm_substance_key {
	uint32_t module_id;
	uint32_t outcome_request_bs32_p;
};

struct zvm_substance_keyval {
	struct zvm_substance_key key;
	uint32_t substance_id;
};

struct zvm_substance {
	struct zvm_substance_key key;
	uint32_t sequence_len;
	uint32_t sequence_p;
	int transmogrified;
	int refcount;
};

struct zvm_program {
	struct zvm_module* modules;
	uint32_t main_module_id;
	uint32_t main_substance_id;
	uint32_t* buf;
	struct zvm_substance_keyval* substance_keyvals;
	struct zvm_substance* substances;
};

struct zvm {
	struct zvm_program prg;
};

extern struct zvm zvmg;
#define ZVM (&zvmg)
#define ZVM_PRG (&ZVM->prg)
#define ZVM_MOD (&ZVM_PRG->modules[zvm_arrlen(ZVM_PRG->modules)-1])

void zvm_init();

void zvm_begin_program();
void zvm_end_program(uint32_t main_module_id);

int zvm_begin_module(int n_inputs, int n_outputs);
int zvm_end_module();

static inline uint32_t zvm_1x(uint32_t x0)
{
	uint32_t idx = zvm_arrlen(ZVM_PRG->buf);
	zvm_arrpush(ZVM_PRG->buf, x0);
	return idx;
}

static inline uint32_t zvm_2x(uint32_t x0, uint32_t x1)
{
	uint32_t idx = zvm_arrlen(ZVM_PRG->buf);
	uint32_t* xs = zvm_arradd(ZVM_PRG->buf, 2);
	xs[0] = x0;
	xs[1] = x1;
	return idx;
}

static inline uint32_t zvm_3x(uint32_t x0, uint32_t x1, uint32_t x2)
{
	uint32_t idx = zvm_arrlen(ZVM_PRG->buf);
	uint32_t* xs = zvm_arradd(ZVM_PRG->buf, 3);
	xs[0] = x0;
	xs[1] = x1;
	xs[2] = x2;
	return idx;
}

static inline uint32_t zvm_op_nor(uint32_t x, uint32_t y)
{
	return zvm_3x(ZVM_OP(NOR), x, y);
}

static inline int zvm__is_valid_module_id(int module_id)
{
	return 0 <= module_id && module_id < ZVM_MAX_OP_ARG && module_id < zvm_arrlen(ZVM_PRG->modules);
}

static inline uint32_t zvm_op_instance(int module_id)
{
	#if DEBUG
	zvm_assert(zvm__is_valid_module_id(module_id));
	#endif
	struct zvm_module* instance_module = &(ZVM_PRG->modules[module_id]);
	ZVM_MOD->n_bits += instance_module->n_bits;
	instance_module->refcount++;
	// expects module->n_inputs zvm_arg()'s following this
	return zvm_1x(ZVM_OP(INSTANCE) | (module_id<<ZVM_OP_BITS));
}

static inline uint32_t zvm_op_unit_delay(uint32_t x)
{
	ZVM_MOD->n_bits++;
	return zvm_2x(ZVM_OP(UNIT_DELAY), x);
}

static inline void zvm_assign_output(int index, uint32_t x)
{
	struct zvm_module* mod = ZVM_MOD;
	zvm_assert(0 <= index && index < mod->n_outputs);
	ZVM_PRG->buf[mod->outputs_p + index] = x;
}

static inline uint32_t zvm_arg(uint32_t arg)
{
	return zvm_1x(arg);
}

static inline int zvm__op_n_args(uint32_t code)
{
	int op = code & ZVM_OP_MASK;
	if (op == ZVM_OP(INSTANCE)) {
		int module_id = (code >> ZVM_OP_BITS);
		#if DEBUG
		zvm_assert(zvm__is_valid_module_id(module_id));
		#endif
		struct zvm_module* mod = &ZVM_PRG->modules[module_id];
		return mod->n_inputs;
	}
	switch (op) {
	#define ZOP(op,narg) case ZVM_OP(op): zvm_assert(narg >= 0); return narg;
	ZVM_OPS
	#undef ZOP
	default: zvm_assert(!"unhandled op"); return 0;
	}
}

static inline int zvm__is_valid_arg_index(uint32_t x, int index)
{
	return 0 <= index && index < zvm__op_n_args(ZVM_PRG->buf[x]);
}

static inline int zvm__arg_index(uint32_t p, int index)
{
	return p+1+index;
}

static inline void zvm_assign_arg(uint32_t x, int index, uint32_t y)
{
	int i = zvm__arg_index(x, index);
	#if DEBUG
	zvm_assert(zvm__is_valid_arg_index(x, index));
	zvm_assert((ZVM_PRG->buf[i] == ZVM_PLACEHOLDER) && "reassignment?");
	#endif
	ZVM_PRG->buf[i] = y;
}

static inline uint32_t zvm_op_unpack(int index, uint32_t x)
{
	#if DEBUG
	zvm_assert(0 <= index && index < ZVM_MAX_OP_ARG);
	#endif
	return zvm_2x(ZVM_OP(UNPACK) | (index<<ZVM_OP_BITS), x);
}

#define ZVM_H
#endif
