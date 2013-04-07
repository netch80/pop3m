CC = gcc
CFLAGS += -g -Wall -Wsign-compare

pop3m: pop3m.c
	$(CC) $(CFLAGS) -o pop3m pop3m.c

clean:
	-rm -f pop3m *~ *.o
