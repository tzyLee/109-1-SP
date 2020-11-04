TARGETS = host player


all: $(TARGETS)

$(TARGETS):%:%.o
	$(CC) -o $(LDFLAGS) $@ $^

$(TARGETS:=.o):%.o:%.c

clean:
	rm -f *.o $(TARGETS)