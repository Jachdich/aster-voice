#include <asio.hpp>
#include <iostream>
#include "audio.c"
#include "stb.h"
#include "stb_vorbis.c"
#define PACKET_SIZE 256
using udp = asio::ip::udp;

uint16_t buf_begin = 0;
uint16_t buf_end = 0;
uint16_t buf_avail = 0;
float *buffer;

void wr_cal(int num_samples, int num_areas, Area* areas) {
    (void)num_samples;
    uint16_t pos_init = buf_begin;
    for (int32_t i = 0; i < num_areas; i++) {
        Area area = areas[i];
        buf_begin = pos_init;
        while (area < area.end) {
            if ((uint16_t)(buf_end - buf_begin) < 1) {
                *area++ = 0.0;
            } else {
                *area++ = buffer[buf_begin++];
            }
        }
    }
}

class udp_server {
public:
    udp_server(asio::io_context &ctx) : socket_(ctx, udp::endpoint(udp::v4(), 2346)) {
        start_recv();
    }
private:
    void start_recv() {
        socket_.async_receive_from(
            asio::buffer(buf), remote_endpoint_,
            [this](const asio::error_code &ec, size_t bytes) {
                this->handle_recv(ec, bytes);
            });
    }

    void handle_recv(const asio::error_code &ec, size_t bytes) {
        if (ec) {
            std::cerr << "ERROR: " << ec.message() << "\n";
        } else {
            (void)bytes;
            int16_t *i16_buf = (int16_t*)buf.data();
            for (uint32_t i = 0; i < bytes / 2; i++) {
                buffer[buf_end] = (float)((double)i16_buf[i] / 32768.0);
                buf_end += 1;
            }
            start_recv();
        }
    }

    udp::socket socket_;
    udp::endpoint remote_endpoint_;
    std::array<uint8_t, PACKET_SIZE> buf;
};

int main() {
    buffer = new float[65536];
    init_audio_client(44100, [](int num_samples, int num_areas, Area* areas) {
        (void)num_samples; (void)num_areas; (void)areas;
    }, wr_cal);

    try {
        asio::io_context ctx;
        udp_server server(ctx);
        ctx.run();
    } catch (std::exception &e) {
        std::cerr << e.what() << "\n";
    }
    return 0;
}
