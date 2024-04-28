CXXFLAGS = -std=c++20

all: main.o
	g++ main.o -o server
main.o: ./src/main.cpp
	g++ -c ./src/main.cpp $(CXXFLAGS)
clean:
	rm -rf *.o
	rm server