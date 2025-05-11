CXX       = g++
CXXFLAGS  = -Wall -O2 -std=c++17 -pthread
LIBS      = -lpq -lssl -lcrypto

all: server client

server: server.cpp db.cpp crypto.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp db.cpp crypto.cpp $(LIBS)

client: client.cpp crypto.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp crypto.cpp $(LIBS)

clean:
	rm -f server client