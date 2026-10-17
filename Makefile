CC     ?= cc
MODE   ?= release
BUILD  := build/$(MODE)

CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Isrc -MMD -MP
LDFLAGS :=

ifeq ($(MODE),debug)
    CFLAGS  += -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address,undefined
else
    CFLAGS  += -O2 -march=native -DNDEBUG
endif

LIB_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(wildcard src/*.c))
PERFT   := $(BUILD)/perft

.PHONY: all debug test test-quick test-full test-debug clean

all: $(PERFT)

$(PERFT): $(BUILD)/tests/perft.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

test: all
	./$(PERFT)

test-quick: all
	./$(PERFT) --quick

test-full: all
	./$(PERFT) --full

# Sanitizers plus a consistency check (bitboards, mailbox, hash) at every perft node.
debug:
	$(MAKE) MODE=debug all

test-debug:
	$(MAKE) MODE=debug test-quick

clean:
	rm -rf build

-include $(LIB_OBJ:.o=.d) $(BUILD)/tests/perft.d
