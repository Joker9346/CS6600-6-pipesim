CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O2

all: pipesim tutorial

pipesim: framework.cc design.cc pipesim.h
	$(CXX) $(CXXFLAGS) framework.cc design.cc -o pipesim

tutorial: tutorial.cc
	$(CXX) $(CXXFLAGS) tutorial.cc -o tutorial

clean:
	rm -f pipesim tutorial
