CC      = gcc
CFLAGS  = -Wall -O2 -pthread
TARGET  = warehouse
DEVICE ?= virtual
SRC = src/main.c src/pqueue.c src/device_$(DEVICE).c src/mempool.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: $(TARGET)
	./$(TARGET)
clean:
	rm -f $(TARGET)
