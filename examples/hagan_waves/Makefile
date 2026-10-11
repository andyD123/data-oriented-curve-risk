CXX ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wno-unused-result
all: scan_wave
scan_wave: scan_wave.cpp
	$(CXX) $(CXXFLAGS) -I../.. $< -o $@
clean:
	rm -f scan_wave *.txt

