TARGETDIR ?= ../../../bin

all : cc1 cpp0 dompcc

install :
	install cc1 cpp0 dompcc ${TARGETDIR}

cpp0 : cc1
	ln -s cc1 cpp0

Clean : clean
	rm -f cc1 cpp0 dompcc

clean :
	rm -f *.o
