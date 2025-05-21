CXX       = g++
CXXFLAGS  = -Wall -O2 -std=c++17 -pthread
LIBS      = -lpq -lssl -lcrypto

all: server

server: src/server.cpp src/db.cpp
	$(CXX) $(CXXFLAGS) -o server src/server.cpp src/db.cpp $(LIBS)

clean:
	rm -f server