
#define CC for your compiler and OPT for your compiler options

#Intel's compiler for use with intel Xeon Phi 5110P
#CC=icc
#OPT   = -O3 -DUSEAFFINITY -mmic
#LIBS = -lpthread

# with GCC on most anything
#CC = gcc -std=gnu99
#OPT = -DUSEAFFINITY -DUSENUMA -O3 
CC=gcc -std=gnu99 
OPT = -DUSEAFFINITY -DUSENUMA -O3 -march=native -mtune=native -ftree-vectorize -funroll-loops -finline-functions -fprefetch-loop-arrays
LIBS = -lpthread  -lnuma

SRCFILES=pstream.c
OBJFILES=pstream.o
                                                                                    
pstream: $(OBJFILES) Makefile
	$(CC) -o pstream $(OPT) pstream.c $(LIBS)

.c.o:
	$(CC) -c $(OPT) $*.c
 
clean:
	/bin/rm -f *.o 

#assumes gnu indent
indent:
	indent -ut -bli0 -ts4 -i3 -ts3 -c3 pstream.c
