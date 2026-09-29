CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

dbms: main.cpp storage.cpp bptree.cpp storage.h bptree.h
	$(CXX) $(CXXFLAGS) -o dbms main.cpp storage.cpp bptree.cpp

run: dbms
	./dbms games.txt

clean:
	rm -f dbms dbms.exe data.disk index.disk
