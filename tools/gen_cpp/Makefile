CXX ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
ROOT := ../..
OUT := $(ROOT)/build/s9-cpp

all: $(OUT)/dump $(OUT)/lift_words

$(OUT)/dump: dump.cpp
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $< -o $@

$(OUT)/lift_words: lift_words.cpp $(wildcard lift_*.h)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $< -o $@

$(OUT)/discover: discover.cpp dump.cpp jump_interpreter.h $(wildcard lift_*.h) $(OUT)/deps/capstone-build/libcapstone.a
	$(CXX) $(CXXFLAGS) -I$(OUT)/deps/capstone-5.0.9/src/include $< $(OUT)/deps/capstone-build/libcapstone.a -o $@

$(OUT)/gen: emit.cpp discover.cpp dump.cpp jump_interpreter.h $(wildcard lift_*.h) $(OUT)/deps/capstone-build/libcapstone.a
	$(CXX) $(CXXFLAGS) -I$(OUT)/deps/capstone-5.0.9/src/include $< $(OUT)/deps/capstone-build/libcapstone.a -o $@

$(OUT)/deps/capstone-build/libcapstone.a:
	bash setup_capstone.sh

.PHONY: all
