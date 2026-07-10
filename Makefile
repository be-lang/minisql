CC      = cc
CFLAGS  = -Wall -Wextra -std=c11 -O2
TARGET  = minisql

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

.PHONY: run test clean
run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./tests/run.sh

clean:
	rm -f $(TARGET) t
