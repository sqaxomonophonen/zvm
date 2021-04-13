#include <stdlib.h>
#include <assert.h>

#include "zvm.h"

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
