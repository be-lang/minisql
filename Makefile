CC      = cc
CFLAGS  = -Wall -Wextra -std=c11 -O2
TARGET  = minisql

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

.PHONY: run test stress clean
run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./tests/run.sh

# real-world SQL checked against sqlite3 (needs sqlite3 + bench data)
stress: $(TARGET)
	./bench/bench.sh >/dev/null 2>&1 || true
	./tests/stress.sh

clean:
	rm -f $(TARGET) t
