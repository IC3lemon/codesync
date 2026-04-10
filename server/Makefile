CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lssl -lcrypto -lpthread -lsqlite3

SRC = server.c websocket.c db.c cJSON.c
OBJ = $(SRC:.c=.o)
TARGET = collab_server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
