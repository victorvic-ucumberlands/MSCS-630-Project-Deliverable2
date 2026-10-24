# Makefile - build the shell on Linux (g++ >= 7, C++17)
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -g
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:src/%.cpp=build/%.o)

all: myshell

myshell: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp $(wildcard src/*.hpp) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

demo: myshell
	./demos/run_demos.sh

clean:
	rm -rf build myshell evidence

.PHONY: all demo clean

# Extra safety net: build with AddressSanitizer + UBSan (output: myshell_asan)
sanitize:
	$(CXX) -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer src/*.cpp -o myshell_asan

.PHONY: sanitize
