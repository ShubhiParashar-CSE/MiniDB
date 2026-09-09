CXX = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -pthread -Iinclude

minidb: src/main.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o minidb

clean:
	rm -rf minidb data *.log

.PHONY: clean
