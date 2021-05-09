all: client server

client: client.cpp
	g++ -std=c++17 -lsoundio -lpthread client.cpp -o client

server: server.cpp
	g++ -std=c++17 -lsoundio -lpthread server.cpp -o server
