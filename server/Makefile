CXX       = g++
CXXFLAGS  = -Wall -O2 -std=c++17 -pthread
LIBS      = -lpq -lssl -lcrypto

all: server

server: server.cpp db.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp db.cpp $(LIBS)

clean:
	rm -f server