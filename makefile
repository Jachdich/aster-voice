all: oc os

oc: opus_client.cpp opus_client.h
	g++ -std=c++17 opus_client.cpp -o oc -lsoundio -lpthread -lm -lopus -g

os: opus_server.cpp
	g++ -std=c++17 opus_server.cpp -o os -lsoundio -lpthread -lm -lopus -g
