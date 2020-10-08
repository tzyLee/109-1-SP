all: read_server write_server

read_server: server.c
	$(CC) server.c -D READ_SERVER -o $@

write_server: server.c
	$(CC) server.c -o $@

clean:
	rm read_server write_server