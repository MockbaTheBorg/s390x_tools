CC ?= cc
CFLAGS ?= -O2 -Wall

all: hprint hpunch hread

hprint: hwrite.c
	$(CC) $(CFLAGS) -DMAXLEN=132 -o hprint hwrite.c

hpunch: hwrite.c
	$(CC) $(CFLAGS) -DMAXLEN=80 -o hpunch hwrite.c

hread: hread.c
	$(CC) $(CFLAGS) -o hread hread.c

clean:
	rm -f hprint hpunch hread
