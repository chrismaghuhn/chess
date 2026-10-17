CC     ?= cc
MODE   ?= release
BUILD  := build/$(MODE)

CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Isrc -pthread -MMD -MP
LDFLAGS := -pthread

ifeq ($(MODE),debug)
    CFLAGS  += -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address,undefined
else
    CFLAGS  += -O2 -march=native -DNDEBUG
endif

LIB_OBJ    := $(patsubst %.c,$(BUILD)/obj/%.o,$(wildcard src/*.c))
ENGINE_OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(wildcard engine/*.c))

ENGINE      := $(BUILD)/engine
PERFT       := $(BUILD)/perft
SEARCH_TEST := $(BUILD)/search-test

.PHONY: all debug test test-quick test-full test-debug bench clean

all: $(ENGINE) $(PERFT) $(SEARCH_TEST)

$(ENGINE): $(ENGINE_OBJ) $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(PERFT): $(BUILD)/obj/tests/perft.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(SEARCH_TEST): $(BUILD)/obj/tests/search.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

test: all
	./$(PERFT)
	./$(SEARCH_TEST)
	sh tests/uci.sh ./$(ENGINE)

test-quick: all
	./$(PERFT) --quick
	./$(SEARCH_TEST)
	sh tests/uci.sh ./$(ENGINE)

test-full: all
	./$(PERFT) --full
	./$(SEARCH_TEST)
	sh tests/uci.sh ./$(ENGINE)

bench: all
	./$(ENGINE) bench

# Sanitizers plus a consistency check (bitboards, mailbox, hash) at every perft node.
debug:
	$(MAKE) MODE=debug all

test-debug:
	$(MAKE) MODE=debug test-quick

clean:
	rm -rf build

-include $(LIB_OBJ:.o=.d) $(ENGINE_OBJ:.o=.d) $(BUILD)/obj/tests/perft.d $(BUILD)/obj/tests/search.d
