#include <asio.hpp>
#include <iostream>
#include "stb.h"
#include "stb_vorbis.c"
#include "audio.c"

using udp = asio::ip::udp;

#define PACKET_SIZE 256

uint16_t buf_begin = 0;
uint16_t buf_end = 0;
float   *buffer;

void rd_cal(int num_samples, int num_areas, Area* areas, udp::socket *sock, udp::endpoint *recver_endpoint) {
    while (areas[0] < areas[0].end) {
        int16_t buf = *areas[0]++;
        sock->send_to(asio::buffer(&buf, 2), *recver_endpoint);
    }
}

int main() {
    buffer = new float[65536];
    try {
        asio::io_context ctx;
        udp::resolver resolver(ctx);
        udp::resolver::query query(udp::v4(), "localhost", "2346"); 
        udp::endpoint recver_endpoint = *resolver.resolve(query).begin();
        udp::endpoint *endpoint_ptr = &recver_endpoint;
        udp::socket socket(ctx);
        udp::socket *sock_ptr = &socket;
        socket.open(udp::v4());
        
        init_audio_client(44100, 
            [sock_ptr, endpoint_ptr](int num_samples, int num_areas, Area* areas) {
                rd_cal(num_samples, num_areas, areas, sock_ptr, endpoint_ptr);
            },

            [](int num_samples, int num_areas, Area* areas) {
                (void)num_samples; (void)num_areas; (void)areas;
            }
        );
        while(1);
/*
        size_t pos = 0;
        uint8_t *raw_buf = (uint8_t*)output;
        while (pos < num_samples * 2 - PACKET_SIZE/2) {
            auto begin = std::chrono::high_resolution_clock::now();
            
            pos += PACKET_SIZE;
            auto end = std::chrono::high_resolution_clock::now();
        } |*/
       // boost::array<char, 128> recv_buf;
       // udp::endpoint sender_endpoint;
    } catch (std::exception &e) {
        std::cerr << e.what() << "\n";
    }
}
