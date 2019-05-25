LIB_DIRS=-L./third_party/openjpeg/lib -L./third_party/asdcplib/lib -lpthread -L./third_party/openssl/lib -L./third_party/libxml2/lib -L./third_party/ffmpeg/lib
INCLUDES=-I./third_party/openjpeg/include -I./third_party/asdcplib/include -I./third_party/libxml2/include/libxml2 -I./third_party/ffmpeg/include
COMP_FLAGS=-O3 -DNDEBUG
LINK_FLAGS=-O3 -DNDEBUG

#to-do: get lssl and lcrypto to link statically -> want to avoid LD_LIBRARY_PATH stuff before running the executable
LIBS=-Wl,-Bstatic -lopenjp2 -lasdcp -las02 -lkumu -lxml2 -lavformat -lavcodec -lswscale -lswresample -lavutil -Wl,-Bdynamic -lssl -lcrypto
OBJS=main.o color.o linked_list.o asdcp.o av_pipeline.o imf.o

# to-do: fix linking of asdcplib so that we can link with gcc
all : ${OBJS} 
		g++ -o imf_fs ${OBJS} ${LIBS} ${LINK_FLAGS} ${LIB_DIRS}

imf.o : imf.c
		gcc -c imf.c ${COMP_FLAGS} ${INCLUDES}

av_pipeline.o : av_pipeline.c
		gcc -c av_pipeline.c ${COMP_FLAGS} ${INCLUDES}

asdcp.o : asdcp.cpp
		g++ -c asdcp.cpp ${COMP_FLAGS} ${INCLUDES}

linked_list.o : linked_list.c
		gcc -c linked_list.c ${COMP_FLAGS} ${INCLUDES}

color.o : color.c
		gcc -c color.c ${COMP_FLAGS} ${INCLUDES}

main.o : main.c
		gcc -c main.c ${COMP_FLAGS} ${INCLUDES}

clean : 
		rm imf_fs *.o
