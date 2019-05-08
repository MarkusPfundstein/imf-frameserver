all : main.o color.o convert.o
		gcc -o imf_fs main.o color.o convert.o -lopenjp2 

convert.o : convert.c
		gcc -c convert.c

color.o : color.c
		gcc -c color.c

main.o : main.c
		gcc -c main.c

clean : 
		rm imf_fs *.o
