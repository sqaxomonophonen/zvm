#ifndef ZVM_H

#include <stdint.h>
#include <assert.h>

#define ZVM_OPS \
	\
	DEFOP(NOR,2) \
	DEFOP(UNIT_DELAY,1) \
	DEFOP(INSTANCE,-1) \
	DEFOP(UNPACK,1) \
	DEFOP(OUTPUT,1)


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
	#define DEFOP(op,narg) ZVM_OP(op),
	ZVM_OPS
	#undef DEFOP
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
#define zvm_arrsetlen(a,n)   ( ((n)>zvm_arrlen(a)) ? ((void)zvm__maybegrow(a,((n)-zvm_arrlen(a)))) : ((a) ? (void)(zvm__len(a)=(n)) : (void)0) )

extern uint32_t* zvm__buf;

void zvm_init();

void zvm_begin_program();
void zvm_end_program(uint32_t main_module_id);

int zvm_begin_module(int n_inputs, int n_outputs);
int zvm_end_module();

static inline uint32_t zvm_1x(uint32_t x0)
{
	uint32_t* xs = zvm_arradd(zvm__buf, 1);
	xs[0] = x0;
	return xs - zvm__buf;
}

static inline uint32_t zvm_2x(uint32_t x0, uint32_t x1)
{
	uint32_t* xs = zvm_arradd(zvm__buf, 2);
	xs[0] = x0;
	xs[1] = x1;
	return xs - zvm__buf;
}

static inline uint32_t zvm_3x(uint32_t x0, uint32_t x1, uint32_t x2)
{
	uint32_t* xs = zvm_arradd(zvm__buf, 3);
	xs[0] = x0;
	xs[1] = x1;
	xs[2] = x2;
	return xs - zvm__buf;
}

static inline uint32_t zvm_op_nor(uint32_t x, uint32_t y)
{
	return zvm_3x(ZVM_OP(NOR), x, y);
}

static inline uint32_t zvm_op_instance(int module_id)
{
	// expects module->n_inputs zvm_arg()'s following this
	return zvm_1x(ZVM_OP(INSTANCE) | (module_id<<ZVM_OP_BITS));
}

static inline uint32_t zvm_op_unit_delay(uint32_t x)
{
	return zvm_2x(ZVM_OP(UNIT_DELAY), x);
}

static inline uint32_t zvm_op_output(int index, uint32_t x)
{
	return zvm_2x((ZVM_OP(OUTPUT) | (index<<ZVM_OP_BITS)), x);
}

static inline uint32_t zvm_arg(uint32_t arg)
{
	return zvm_1x(arg);
}

static inline int zvm__arg_index(uint32_t p, int index)
{
	return p+1+index;
}

static inline void zvm_assign_arg(uint32_t x, int index, uint32_t y)
{
	int i = zvm__arg_index(x, index);
	#if DEBUG
	//zvm_assert(zvm__is_valid_arg_index(x, index));
	zvm_assert((zvm__buf[i] == ZVM_PLACEHOLDER) && "reassignment?");
	#endif
	zvm__buf[i] = y;
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
