#
# Copyright (c) 2019, LabN Consulting, L.L.C.
# All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
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
