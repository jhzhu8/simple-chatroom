CC = gcc
CFLAGS = -Wall -fstrict-aliasing -g

INCLUDES = -I./

SRC = main.c chatroom.c

LIBS = -lpthread

OBJS = $(SRC: .c=.o)

MAIN = chat_server

.PHONY: depend clean

all: $(MAIN)

$(MAIN): $(OBJS)
		$(CC) $(CFLAGS) $(INCLUDES) -o $(MAIN) $(OBJS) $(LIBS)

.c.o:
		$(CC) $(CFLAGS) $(INCLUDES) -c $<  -o $@

clean:
		$(RM) *.o *~ $(MAIN)

depend: $(SRC)
		makedepend $(INCLUDES) $^
	
# DO NOT DELETE THIS LINE -- make depend needs it