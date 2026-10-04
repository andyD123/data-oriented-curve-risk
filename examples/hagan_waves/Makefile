# Alternative to CMake. Requires QuantLib headers and library (e.g. apt install libquantlib0-dev).
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wno-unused-result
all: ql_waves scan_wave
ql_waves: ql_waves.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ -lQuantLib
scan_wave: scan_wave.cpp
	$(CXX) $(CXXFLAGS) -I../.. $< -o $@
clean:
	rm -f ql_waves scan_wave *.txt
