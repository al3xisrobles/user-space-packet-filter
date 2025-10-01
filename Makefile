CXX ?= c++
CXXFLAGS := -O3 -march=native -std=c++20 -Wall -Wextra -DNDEBUG
CPPFLAGS := -Iinclude -DUSE_NETMAP -DNETMAP_WITH_LIBS
LDLIBS   := -lnetmap -lpthread

SRC := src/main.cpp src/bypass_io.cpp src/packet_capture.cpp src/packet_filter.cpp src/benchmarks.cpp src/trading_engine.cpp
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC))
BIN := build/user_space_packet_filter

SUBDIRS := utils

all: $(BIN) $(SUBDIRS)


$(SUBDIRS):
	$(MAKE) -C $@

$(BIN): $(OBJ) | build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OBJ) -o $@ $(LDLIBS)

# Compile each .cpp in src/ into a matching .o in build/
build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

.PHONY: all clean
