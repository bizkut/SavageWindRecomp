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

.PHONY: all
