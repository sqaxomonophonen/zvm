#ifndef ZVM_H

#include <stdint.h>

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

struct zvm_module {
	uint32_t* nodes;
};

struct zvm {
	struct zvm_module* modules;
};


static inline int zvm_begin_module(struct zvm* zvm)
{
	int index = zvm_arrlen(zvm->modules);
	struct zvm_module m = {0};
	zvm_arrpush(zvm->modules, m);
	return index;
}

static inline void zvm_end_module(struct zvm* zvm)
{
}

#define ZVM_H
#endif
