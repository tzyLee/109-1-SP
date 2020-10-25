all: read_server write_server

read_server: server.c
	$(CC) server.c -D READ_SERVER -D NDEBUG -o $@

write_server: server.c
	$(CC) server.c -D NDEBUG -o $@

clean:
	rm read_server write_server