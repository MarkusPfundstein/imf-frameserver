all : main.o color.o io.o linked_list.o
		gcc -o imf_fs main.o color.o io.o linked_list.o -lopenjp2 

linked_list.o : linked_list.c
		gcc -c linked_list.c

io.o : io.c
		gcc -c io.c

color.o : color.c
		gcc -c color.c

main.o : main.c
		gcc -c main.c

clean : 
		rm imf_fs *.o
