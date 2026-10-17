CC     ?= cc
MODE   ?= release
BUILD  := build/$(MODE)
TMP    := $(BUILD)/tmp

# -iquote: src/features.h must not shadow the system's <features.h>.
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -iquote src -pthread -MMD -MP
LDFLAGS := -pthread
LDLIBS  := -lm

ifeq ($(MODE),debug)
    CFLAGS  += -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address,undefined
else
    CFLAGS  += -O2 -march=native -DNDEBUG
endif

LIB_OBJ    := $(patsubst %.c,$(BUILD)/obj/%.o,$(wildcard src/*.c))
ENGINE_OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(wildcard engine/*.c))
TEST_OBJ   := $(patsubst %.c,$(BUILD)/obj/%.o,$(wildcard tests/*.c))

ENGINE      := $(BUILD)/engine
PERFT       := $(BUILD)/perft
SEARCH_TEST := $(BUILD)/search-test
EVAL_TEST   := $(BUILD)/eval-test

.PHONY: all debug test test-quick test-full test-debug bench clean

all: $(ENGINE) $(PERFT) $(SEARCH_TEST) $(EVAL_TEST)

$(ENGINE): $(ENGINE_OBJ) $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERFT): $(BUILD)/obj/tests/perft.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(SEARCH_TEST): $(BUILD)/obj/tests/search.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(EVAL_TEST): $(BUILD)/obj/tests/eval.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Everything after perft; eval-test leaves the weight file the self-play test uses.
define other_tests
	./$(SEARCH_TEST)
	@mkdir -p $(TMP)
	./$(EVAL_TEST) $(TMP)
	sh tests/uci.sh ./$(ENGINE)
	sh tests/selfplay.sh ./$(ENGINE) $(TMP)
endef

test: all
	./$(PERFT)
	$(other_tests)

test-quick: all
	./$(PERFT) --quick
	$(other_tests)

test-full: all
	./$(PERFT) --full
	$(other_tests)

bench: all
	./$(ENGINE) bench

# Sanitizers plus a consistency check (bitboards, mailbox, hash) at every perft node.
debug:
	$(MAKE) MODE=debug all

test-debug:
	$(MAKE) MODE=debug test-quick

clean:
	rm -rf build

-include $(LIB_OBJ:.o=.d) $(ENGINE_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
