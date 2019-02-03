
OBJDIR ?= build
SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))
BIN := $(OBJDIR)/iptfs

all: $(BIN)

clean: rm $(BIN) $(OBJ)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BIN): $(OBJDIR) $(OBJ)
	gcc -o $@ $(OBJ) -lpthread

$(OBJ): $(SRC)

$(OBJDIR)/%.o: src/%.c src/iptfs.h
	gcc -c -o $@ $<
