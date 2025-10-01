CXX := g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -DNETMAP_WITH_LIBS
LDFLAGS := -lnetmap -pthread

all: nm_md_sender

nm_md_sender: nm_md_sender.cpp
	$(CXX) $(CXXFLAGS) nm_md_sender.cpp -o nm_md_sender $(LDFLAGS)

clean:
	rm -f nm_md_sender

.PHONY: all clean
