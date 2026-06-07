CC      = gcc
CFLAGS  = -Wall -O2 -pthread
TARGET  = warehouse

# 預設虛擬裝置(VM 模擬);樹莓派上改用:make DEVICE=gpio
DEVICE ?= virtual

SRC = src/main.c src/pqueue.c src/device_$(DEVICE).c src/mempool.c

ifeq ($(DEVICE),gpio)
LDLIBS = -lgpiod
else
LDLIBS =
endif

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
