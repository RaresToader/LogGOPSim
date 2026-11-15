CXX=g++
CC=cc
# optimization level for every binary; benchmark builds use OPT="-O3 -DNDEBUG"
OPT ?= -O0 -g
CXXFLAGS= -std=c++11 $(OPT) -pedantic -Wno-deprecated -Wall -Wno-long-long -I/opt/homebrew/include/
CCFLAGS= $(OPT)
LDFLAGS= -L/opt/homebrew/lib/ -lcgraph -g

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LDLIBS += -ldl
endif

# rebuild everything when OPT changes (a stamp file named after the flags)
OPT_STAMP := .opt-$(shell printf '%s' '$(OPT)' | cksum | cut -d' ' -f1)
$(OPT_STAMP):
	rm -f .opt-*
	touch $@

AUTOGEN_SRC= loggopsim_cmdline.c loggopsim_cmdline.h txt2bin_cmdline.h txt2bin_cmdline.c
LOGGOPSIM_OBJECTS= LogGOPSim.o
HLPR_OBJECTS= loggopsim_cmdline.o
ALL_OBJECTS= $(LOGGOPSIM_OBJECTS) $(HLPR_OBJECTS)
BINARY= LogGOPSim

all: $(BINARY) txt2bin goalx-convert

$(BINARY): $(ALL_OBJECTS) $(OPT_STAMP)
	$(CXX) $(CXXFLAGS) $(ALL_OBJECTS) -o $(BINARY) $(LDFLAGS) $(LDLIBS)

# commit and flags baked into LogGOPSim for --emit-results; build_info.h changes only when they do
build_info.h: FORCE
	@printf '#define GOALX_BUILD_COMMIT "%s"\n#define GOALX_BUILD_FLAGS "%s"\n' \
	  "$$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$$(git diff --quiet HEAD -- . 2>/dev/null || echo -dirty)" \
	  "$(CXX) $(OPT)" > build_info.h.tmp
	@cmp -s build_info.h.tmp build_info.h || cp build_info.h.tmp build_info.h

LogGOPSim.o: build_info.h
FORCE:

txt2bin:
	re2c -o txt2bin.cpp txt2bin.re
	gengetopt -F txt2bin_cmdline -i txt2bin_cmdline.ggo
	$(CXX) -std=c++11 $(OPT) txt2bin.cpp txt2bin_cmdline.c -o txt2bin

goalx-convert: goalx_convert.cpp goalx_convert_cmdline.c goalx_convert_cmdline.h *.hpp *.h $(OPT_STAMP)
	$(CXX) $(CXXFLAGS) goalx_convert.cpp goalx_convert_cmdline.c -o goalx-convert $(LDLIBS)

goalx_convert_cmdline.c goalx_convert_cmdline.h: goalx_convert_cmdline.ggo
	gengetopt -F goalx_convert_cmdline -i goalx_convert_cmdline.ggo

liblegacy_plugin.so: ../../examples/decoder_plugin/legacy_plugin.c goalx_decoder.h $(OPT_STAMP)
	$(CC) -std=c99 $(OPT) -Wall -shared -fPIC -I. ../../examples/decoder_plugin/legacy_plugin.c -o liblegacy_plugin.so

loggopsim_cmdline.c: loggopsim_cmdline.ggo
	gengetopt -F loggopsim_cmdline -i loggopsim_cmdline.ggo

loggopsim_cmdline.h: loggopsim_cmdline.ggo
	gengetopt -F loggopsim_cmdline -i loggopsim_cmdline.ggo

txt2bin_cmdline.c: txt2bin_cmdline.ggo
	gengetopt -F txt2bin_cmdline -i txt2bin_cmdline.ggo

txt2bin_cmdline.h: txt2bin_cmdline.ggo
	gengetopt -F txt2bin_cmdline -i txt2bin_cmdline.ggo



%.o: %.cpp $(AUTOGEN_SRC) *.hpp *.h $(OPT_STAMP)
	$(CXX) $(CXXFLAGS) -c $<

%.o: %.c $(AUTOGEN_SRC) *.h $(OPT_STAMP)
	$(CXX) $(CCFLAGS) -c $<

clean:
	rm -f $(AUTOGEN_SRC)
	rm -f $(ALL_OBJECTS)
	rm -f $(BINARY)
	rm -f txt2bin.cpp txt2bin bin2txt bin2dot simtest liblegacy_plugin.so goalx-convert
	rm -f cmdline_txt2bin.* .opt-* build_info.h build_info.h.tmp

.PHONY: all txt2bin clean FORCE
