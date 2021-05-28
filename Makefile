#OPT=-O2
OPT=-O0 -g
CFLAGS=-std=c99 -Wall $(OPT) -DDEBUG -DVERBOSE_DEBUG

bin=test_basic test_ram

all: $(bin)

zvm.o: zvm.c zvm.h

test_basic.o: test_basic.c zvm.h
test_ram.o: test_ram.c zvm.h

test_basic: test_basic.o zvm.o
test_ram: test_ram.o zvm.o

clean:
	rm -f *.o $(bin)

