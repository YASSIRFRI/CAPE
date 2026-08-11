CFLAGS += -O3
LDLIBS += -ll
LFLAGS += -d
YFLAGS += -d

all : ompcc

ompcc : parser.o lexer.o ompcc.o

Clean : clean
	rm -rf ompcc

clean :
	rm -rf *.o y.tab.h
