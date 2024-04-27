CPP_FLAGS = -std=c++20
HEADERS = http.hpp

all: main.o
	g++ main.o -o server
main.o: main.cpp $(HEADERS)
	g++ -c main.cpp $(CPP_FLAGS)
clean:
	rm -rf *.o
	rm server