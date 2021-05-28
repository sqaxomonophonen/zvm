#OPT=-O2
OPT=-O0 -g
CFLAGS=-std=c99 -Wall $(OPT) -DDEBUG -DVERBOSE_DEBUG

bin=t1zvm test_basic

all: $(bin)

zvm.o: zvm.c zvm.h
t1zvm.o: t1zvm.c zvm.h
test_basic.o: test_basic.c zvm.h

t1zvm: t1zvm.o zvm.o
test_basic: test_basic.o zvm.o

clean:
	rm -f *.o $(bin)

