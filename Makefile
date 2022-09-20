CC	?= cc
CFLAGS	?= -DPOSIX_C_SOURCE=200112L -std=c99 -Wall -Werror -pthread -O1

SRC	?= tetris.c

DST	?= tetris
DST_DIR ?= /usr/local/bin

all: $(DST)

$(DST):
	$(CC) $(CFLAGS) -o $(DST) $(SRC)

clean:
	rm -f *.o $(DST)

install: $(DST)
	ln -sf $(DST) $(DST_DIR)

uninstall:
	rm $(DST_DIR)/$(DST)

.PHONY: all clean install uninstall
