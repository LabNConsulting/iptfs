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
PYCFG := setup.cfg
PYSRC := $(wildcard iptfs/*.py)
SRC := $(wildcard src/*.c)
INC := $(wildcard src/*.h)
OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))
BIN := $(OBJDIR)/iptfs

ORG := $(shell find -name '*.org')
DRAFTS := draft/draft-chopps-ipsecme-iptfs-00.txt draft/draft-chopps-ipsecme-iptfs-00.xml
SCRIPTS := $(wildcard *.sh)
MAKEFILES := $(shell find -name '*.org')

all: $(BIN)

print: $(ALL)
	enscript -o - -1 $(DRAFTS) $(ORG) | ps2pdf - release-pack-1pp.pdf
	enscript -r -o - -1 $(PYSRC) | ps2pdf - release-pack-1pp-wide.pdf
	enscript -r -o - -2 $(SRC) $(INC) $(MAKEFILES) $(SCRIPTS) | ps2pdf - release-pack-2pp.pdf
	gs -dBATCH -dNOPAUSE -q -sDEVICE=pdfwrite -sOutputFile=release-pack.pdf file1.pdf file2.pdf


clean:
	rm -f $(BIN) $(OBJ) release-pack-*.pdf

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BIN): $(OBJDIR) $(OBJ)
	gcc -o $@ $(OBJ) -lpthread

$(OBJ): $(SRC)

$(OBJDIR)/%.o: src/%.c src/iptfs.h
	gcc -c -o $@ $<
