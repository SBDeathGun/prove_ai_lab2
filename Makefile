CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Iinclude
AR ?= ar
ARFLAGS ?= rcs

SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
EXAMPLES_DIR = examples
TESTS_DIR = tests

SRCS = $(SRC_DIR)/mr_api.c \
       $(SRC_DIR)/mr_common.c \
       $(SRC_DIR)/mr_queue.c \
       $(SRC_DIR)/mr_mapper.c \
       $(SRC_DIR)/mr_reducer.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

LIB = libmr.a
EXAMPLES = $(EXAMPLES_DIR)/word_count $(EXAMPLES_DIR)/dump_output
TEST_BIN = $(TESTS_DIR)/test_suite

.PHONY: all test clean

all: $(LIB) $(EXAMPLES)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(EXAMPLES_DIR)/word_count: $(EXAMPLES_DIR)/word_count.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lmr -o $@

$(EXAMPLES_DIR)/dump_output: $(EXAMPLES_DIR)/dump_output.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lmr -o $@

$(TEST_BIN): $(TESTS_DIR)/test_suite.c $(LIB)
	$(CC) $(CFLAGS) -Isrc $< -L. -lmr -o $@

test: $(TEST_BIN)
	@echo "Esecuzione della suite di test..."
	./$(TEST_BIN)

clean:
	rm -rf $(OBJ_DIR) $(LIB) $(EXAMPLES) $(TEST_BIN) *.mro *.log *.stats /tmp/test_mr_*
