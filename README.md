# SC3020 Project 1: Storage and B+ Tree Index

## Files

| File | Purpose |
|---|---|
| `storage.h`, `storage.cpp` | Task 1: record layout, simulated disk, database file |
| `bptree.h`, `bptree.cpp` | Task 2 and 3: disk based B+ tree (bulk load, range search, deletion) |
| `main.cpp` | Runs all three tasks and prints the result tables |
| `Makefile` | Build script for Linux and macOS |

## Requirements

A C++17 compiler: g++ 7 or newer, clang 5 or newer, or Visual Studio 2017 or newer. No external libraries are needed.

## Setup

1. Download the dataset from the link in the project brief and save it as `games.txt` in this folder.
2. Build the program using one of the options below.

**Linux / macOS**

```
make
```

**Windows (MinGW g++)**

```
g++ -std=c++17 -O2 -o dbms.exe main.cpp storage.cpp bptree.cpp
```

**Windows (Visual Studio Developer Command Prompt)**

```
cl /std:c++17 /O2 /EHsc main.cpp storage.cpp bptree.cpp /Fe:dbms.exe
```

## Running

```
./dbms games.txt              (default block size of 4096 bytes)
./dbms games.txt 1024         (custom block size)
./dbms games.txt 4096 3       (cap n at 3 to see a deep tree; prints it if small)
```

On Windows, use `dbms.exe` in place of `./dbms`.

The program creates two files in the current folder:

* `data.disk`: the database file holding the records (Task 1)
* `index.disk`: the B+ tree, one node per block (Task 2)

Both are rebuilt from scratch on every run, so the program can be run repeatedly.

## Output

1. Task 1: record size, number of records, records per block, number of blocks
2. Task 2: n, number of nodes, number of levels, root node keys
3. Task 3: index nodes and data blocks accessed, games deleted, average FG_PCT_home, running time, the brute force comparison, and the updated tree statistics
4. Consistency checks confirming the deletion was complete and the tree is still valid
