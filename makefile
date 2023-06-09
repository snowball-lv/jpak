
BIN = bin/jpak
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:src/%.c=out/%.o)
DEPS = $(SRCS:src/%.c=out/%.d)

CFLAGS = -c -MMD -I inc -Wall -O2

all: $(BIN)

-include $(DEPS)

out:
	mkdir out

out/%.o: src/%.c | out
	$(CC) $(CFLAGS) $< -o $@

bin:
	mkdir bin

$(BIN): $(OBJS) | bin
	$(CC) $^ -o $@

clean:
	rm -rf out bin

test: all
	$(BIN) test.json
	# cat test.json | $(BIN)
	$(BIN) -d test.dict -b test.bj -o output.json -g
