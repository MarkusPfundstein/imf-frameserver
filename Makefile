COMP_FLAGS=-O3 -DNDEBUG
LINK_FLAGS=-O3 -DNDEBUG

# to-do: fix linking of asdcplib so that we can link with gcc
all : main.o asdcp.o color.o io.o linked_list.o
		g++ -o imf_fs main.o color.o io.o linked_list.o asdcp.o -lopenjp2 -lasdcp -las02 -lkumu ${LINK_FLAGS}

asdcp.o : asdcp.cpp
		g++ -c asdcp.cpp ${COMP_FLAGS}

linked_list.o : linked_list.c
		gcc -c linked_list.c ${COMP_FLAGS}

io.o : io.c
		gcc -c io.c ${COMP_FLAGS}

color.o : color.c
		gcc -c color.c ${COMP_FLAGS}

main.o : main.c
		gcc -c main.c ${COMP_FLAGS}

clean : 
		rm imf_fs *.o
