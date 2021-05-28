#ifndef ZVM_H

#include <stdint.h>
#include <assert.h>

#define ZVM_OPS \
	\
	ZOP(NIL,-1) \
	ZOP(A21,2) \
	ZOP(A11,1) \
	ZOP(UNIT_DELAY,1) \
	ZOP(INSTANCE,-1) \
	ZOP(INPUT,0) \
	ZOP(OUTPUT,1) \
	ZOP(CONST,0) \
	ZOP(N,-1)

#define ZVM_OP(op) ZVM_OP_##op
#define ZVM_A11_OP(op) ZVM_A11_OP_##op
#define ZVM_A21_OP(op) ZVM_A21_OP_##op

// arithmetic 2-to-1 ops
#define ZVM_A21_OPS \
	\
	ZOP(NIL) \
	ZOP(NOR) \
	ZOP(NAND) \
	ZOP(OR) \
	ZOP(AND) \
	ZOP(XOR) \
	ZOP(N)

// arithmetic 1-to-1 ops
#define ZVM_A11_OPS \
	\
	ZOP(NIL) \
	ZOP(NOT) \
	ZOP(N)

struct zvm_pi {
	uint32_t p;
	uint32_t i;
};


#define ZVM_NIL             ((uint32_t)-1)
#define ZVM_PLACEHOLDER     ZVM_NIL
#define ZVM_PI_PLACEHOLDER  zvm_pi(ZVM_PLACEHOLDER,ZVM_PLACEHOLDER)

static inline struct zvm_pi zvm_pi(uint32_t p, uint32_t i)
{
	return (struct zvm_pi) { .p=p, .i=i };
}

static inline struct zvm_pi zvm_p0(uint32_t p)
{
	return zvm_pi(p,0);
}

static inline struct zvm_pi zvm_pn(uint32_t p)
{
	return zvm_pi(p,ZVM_NIL);
}

static inline struct zvm_pi zvm_pii(struct zvm_pi pi, uint32_t new_i)
{
	return zvm_pi(pi.p, new_i);
}

#define ZVM_OP_BITS (8)
#define ZVM_OP_MASK ((1<<ZVM_OP_BITS )-1)
#define ZVM_MAX_OP_ARG (1<<(32-ZVM_OP_BITS))
#define ZVM_OP_ENCODE_XY(x,y) (((x) & ZVM_OP_MASK) | (((y) & ((1<<(32-ZVM_OP_BITS))-1)) << ZVM_OP_BITS))
#define ZVM_OP_DECODE_X(op) ((op) & ZVM_OP_MASK)
#define ZVM_OP_DECODE_Y(op) ((op) >> ZVM_OP_BITS)

#define ZVM_ARRAY_LENGTH(x) (sizeof(x)/sizeof(x[0]))

#define zvm_assert assert

enum zvm_ops {
	#define ZOP(op,narg) ZVM_OP(op),
	ZVM_OPS
	#undef ZOP
};

enum zvm_a21_ops {
	#define ZOP(op) ZVM_A21_OP(op),
	ZVM_A21_OPS
	#undef ZOP
};

enum zvm_a11_ops {
	#define ZOP(op) ZVM_A11_OP(op),
	ZVM_A11_OPS
	#undef ZOP
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

void zvm_run(int* arguments, int* retvals);

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

static inline uint32_t zvm_5x(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3, uint32_t x4)
{
	uint32_t* xs = zvm_arradd(zvm__buf, 5);
	xs[0] = x0;
	xs[1] = x1;
	xs[2] = x2;
	xs[3] = x3;
	xs[4] = x4;
	return xs - zvm__buf;
}

static inline struct zvm_pi zvm_op21(uint32_t op, struct zvm_pi x, struct zvm_pi y)
{
	return zvm_p0(zvm_5x(op, x.p, x.i, y.p, y.i));
}

static inline struct zvm_pi zvm_op11(uint32_t op, struct zvm_pi x)
{
	return zvm_p0(zvm_3x(op, x.p, x.i));
}

static inline struct zvm_pi zvm_op_a21(uint32_t aop, struct zvm_pi x, struct zvm_pi y)
{
	return zvm_op21(ZVM_OP_ENCODE_XY(ZVM_OP(A21), aop), x, y);
}

static inline struct zvm_pi zvm_op_nor(struct zvm_pi x, struct zvm_pi y)
{
	return zvm_op_a21(ZVM_A21_OP(NOR), x, y);
}

static inline struct zvm_pi zvm_op_instance(int module_id)
{
	// expects module->n_inputs zvm_arg()'s following this
	return zvm_pn(zvm_1x(ZVM_OP_ENCODE_XY(ZVM_OP(INSTANCE), module_id)));
}

static inline struct zvm_pi zvm_op_unit_delay(struct zvm_pi x)
{
	return zvm_op11(ZVM_OP(UNIT_DELAY), x);
}

static inline struct zvm_pi zvm_op_input(int index)
{
	return zvm_p0(zvm_1x(ZVM_OP_ENCODE_XY(ZVM_OP(INPUT), index)));
}

static inline struct zvm_pi zvm_op_output(int index, struct zvm_pi x)
{
	return zvm_pn(zvm_3x(ZVM_OP_ENCODE_XY(ZVM_OP(OUTPUT), index), x.p, x.i));
}

static inline struct zvm_pi zvm_op_const(int v)
{
	return zvm_p0(zvm_1x(ZVM_OP_ENCODE_XY(ZVM_OP(CONST), v)));
}

static inline uint32_t zvm_arg(struct zvm_pi x)
{
	return zvm_2x(x.p, x.i);
}

static inline int zvm__arg_index(uint32_t p, int index)
{
	return p+1+(index<<1);
}

static inline void zvm_assign_arg(uint32_t p, int index, struct zvm_pi x)
{
	int ai = zvm__arg_index(p, index);
	#if DEBUG
	//zvm_assert(zvm__is_valid_arg_index(x, index));
	zvm_assert((zvm__buf[ai] == ZVM_PLACEHOLDER) && "reassignment?");
	zvm_assert((zvm__buf[ai+1] == ZVM_PLACEHOLDER) && "reassignment?!");
	#endif
	zvm__buf[ai] = x.p;
	zvm__buf[ai+1] = x.i;
}

#define ZVM_H
#endif
