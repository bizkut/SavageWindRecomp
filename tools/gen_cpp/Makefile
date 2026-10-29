CXX ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
ROOT := ../..
OUT := $(ROOT)/build/s9-cpp

all: $(OUT)/dump

$(OUT)/dump: dump.cpp
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $< -o $@

.PHONY: all
