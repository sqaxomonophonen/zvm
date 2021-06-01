#OPT=-O2
OPT=-O0 -g
F=-DDEBUG -DVERBOSE_DEBUG
#F=-DDEBUG
CFLAGS=-std=c99 -Wall $(OPT) $(F)

bin=test_basic test_ram test_ram2

all: $(bin)

zvm.o: zvm.c zvm.h

test_basic.o: test_basic.c zvm.h
test_ram.o: test_ram.c zvm.h
test_ram2.o: test_ram2.c zvm.h

test_basic: test_basic.o zvm.o
test_ram: test_ram.o zvm.o
test_ram2: test_ram2.o zvm.o

clean:
	rm -f *.o $(bin)

