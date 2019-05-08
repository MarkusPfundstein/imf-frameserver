LIBS=-lopenjp2
OBJS=main.o

all : main.o
		gcc -o imf_fs ${OBJS} ${LIBS}

main.o : main.c
		gcc -c main.c

clean : 
		rm imf_fs
