
OBJDIR ?= build
SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))
BIN := $(OBJDIR)/tcptfs

all: $(BIN)

clean: rm $(BIN) $(OBJ)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BIN): $(OBJDIR) $(OBJ)
	gcc -o $@ $(OBJ) -lpthread

$(OBJ): $(SRC)

$(OBJDIR)/%.o: src/%.c
	gcc -c -o $@ $<
