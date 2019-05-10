LIBS=-L./third_party/openjpeg/lib -L./third_party/asdcplib/lib -lpthread -L./third_party/openssl/lib
INCLUDES=-I./third_party/openjpeg/include -I./third_party/asdcplib/include
COMP_FLAGS=-O3 -DNDEBUG
LINK_FLAGS=-O3 -DNDEBUG

# to-do: fix linking of asdcplib so that we can link with gcc
all : main.o asdcp.o color.o io.o linked_list.o
		g++ -o imf_fs main.o color.o io.o linked_list.o asdcp.o -Wl,-Bstatic -lopenjp2 -lasdcp -las02 -lkumu -Wl,-Bdynamic -lssl -lcrypto ${LINK_FLAGS} ${LIBS}

asdcp.o : asdcp.cpp
		g++ -c asdcp.cpp ${COMP_FLAGS} ${INCLUDES}

linked_list.o : linked_list.c
		gcc -c linked_list.c ${COMP_FLAGS} ${INCLUDES}

io.o : io.c
		gcc -c io.c ${COMP_FLAGS} ${INCLUDES}

color.o : color.c
		gcc -c color.c ${COMP_FLAGS} ${INCLUDES}

main.o : main.c
		gcc -c main.c ${COMP_FLAGS} ${INCLUDES}

clean : 
		rm imf_fs *.o
