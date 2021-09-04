#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <opus/opus.h>
#include <stdio.h>
#include <soundio/soundio.h>
#include <math.h>
#include <asio.hpp>
#include <deque>
#include <mutex>
#include <iostream>
#include <thread>

#define FRAME_SIZE 960
#define SAMPLE_RATE 48000
#define APPLICATION OPUS_APPLICATION_AUDIO
#define BITRATE 64000

#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE (3*1276)
using udp = asio::ip::udp;

enum {
    REQ_ACK,
    REQ_ADATA,
    REQ_IDENT,
    REQ_END
};

OpusEncoder *setup_opus_encoder() {
    int err;
    OpusEncoder *enc;
    enc = opus_encoder_create(48000, 1, APPLICATION, &err);
    if (err < 0) {
        fprintf(stderr, "failed to create encoder: %s\n", opus_strerror(err));
        exit(1);
    }

    err = opus_encoder_ctl(enc, OPUS_SET_BITRATE(BITRATE));
    if (err < 0) {
        fprintf(stderr, "failed to set bitrate: %s\n", opus_strerror(err));
        exit(1);
    }
    return enc;
}

OpusDecoder *setup_opus_decoder() {
    int err;
    OpusDecoder *dec;
    dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
    if (err < 0) {
        fprintf(stderr, "failed to create decoder: %s\n", opus_strerror(err));
        exit(1);
    }
    return dec;
}
struct Connection {
    OpusDecoder *dec;
    OpusEncoder *enc;
    std::deque<opus_int16> inbuf;
    std::mutex inbuf_m;
    udp::endpoint endpt;
    uint64_t uuid;
    uint64_t iters_without_msg;
    Connection(udp::endpoint endp) {
        endpt = endp;
        dec = setup_opus_decoder();
        enc = setup_opus_encoder();
        uuid = 0;
        iters_without_msg = 0;
    }
};

class udp_server {
public:
    udp_server(asio::io_context &ctx) : socket_(ctx, udp::endpoint(udp::v4(), 2346)) {
        start_recv();
    }
private:
    void start_recv() {
        socket_.async_receive_from(
            asio::buffer(cbytes, MAX_PACKET_SIZE + 1), remote_endpoint_,
            [this](const asio::error_code &ec, size_t bytes) {
                this->handle_recv(ec, bytes);
            });
    }

    void handle_recv(const asio::error_code &ec, size_t nBytes) {
        if (ec) {
            fprintf(stderr, "ERROR: %s\n", ec.message().c_str());
        } else {
            Connection *conn = NULL;
            int idx = 0;
            for (Connection *c : conns) {
                if (c->endpt == remote_endpoint_) {
                    conn = c;
                    break;
                }
                idx++;
            }
            if (conn == NULL) {
                idx = conns.size();
                Connection *c = new Connection(remote_endpoint_);
                conns.push_back(c);
                conn = conns.back();
            }

            uint8_t req = cbytes[0];
            if (req == REQ_IDENT) {
                memcpy(&conn->uuid, cbytes + 1, 8);
                uint8_t ack_msg[1] = {REQ_ACK};
                socket_.send_to(asio::buffer(ack_msg, 1), conn->endpt);
            } else if (req == REQ_END) {
                if (conn->uuid != 0) {
                    uint8_t ack_msg[1] = {REQ_ACK};
                    socket_.send_to(asio::buffer(ack_msg, 1), conn->endpt);
                    delete conn;
                    conns.erase(std::next(conns.begin(), idx));
                } else {
                    printf("END req without identifying first\n");
                }
            } else if (req == REQ_ADATA) {
                if (conn->uuid != 0 || true) {
                    conn->iters_without_msg = 0;
                    opus_int16 out[MAX_FRAME_SIZE];
                    int frame_size = opus_decode(conn->dec, cbytes + 1, nBytes - 1, out, MAX_FRAME_SIZE, 0);
                    //for (int i = 0; i < frame_size; i++) {
                    //    uint8_t *v = (uint8_t*)(out + i);
                    //    uint8_t temp = v[0];
                    //    v[0] = v[1];
                    //    v[1] = temp;
                    //}
                    if (frame_size < 0) {
                        fprintf(stderr, "decoder failed: %s\n", opus_strerror(frame_size));
                    } else {
                        std::lock_guard<std::mutex> lock(conn->inbuf_m);
                        for (int i = 0; i < frame_size; i++) {
                            conn->inbuf.push_front(out[i]);
                        }
                        printf("Inbuf %d %d\n", conn->uuid, conn->inbuf.size());
                    }
                } else {
                    printf("ADATA req without identifying first\n");
                }
            }
            //std::cout << "sending to " << remote_endpoint_ << "\n";
            //socket_.send_to(asio::buffer(cbytes, nBytes), remote_endpoint_);
            start_recv();
        }
    }
public:
    void calculate_shit() {
        while (true) {
            unsigned long long startns = std::chrono::duration_cast< std::chrono::microseconds >(
            	    	std::chrono::system_clock::now().time_since_epoch()).count();
            int idx = 0;
            for (Connection *c : conns) {
                //printf("Outbuf: %d %d", c->uuid, c->outbuf.size());
                c->iters_without_msg++;
                if (c->iters_without_msg > (SAMPLE_RATE * 16) / FRAME_SIZE) {
                    delete c;
                    conns.erase(std::next(conns.begin(), idx));
                    break;
                }
                opus_int16 combined[FRAME_SIZE] = {};
                int num = 0;
                for (Connection *co : conns) {
                    std::unique_lock<std::mutex> lck(co->inbuf_m);
                    if (co->inbuf.size() >= FRAME_SIZE * 2 && co != c) {
                        num++;
                    } else if (co->inbuf.size() < FRAME_SIZE * 2) {
                        printf("Skipping with %d frames\n", co->inbuf.size());
                    }
                    lck.unlock();
                }
                if (num != 0) {
                    for (Connection *co : conns) {
                        std::unique_lock<std::mutex> lck(co->inbuf_m);
                        if (co->inbuf.size() >= FRAME_SIZE * 2 && co != c) {
                            for (int i = 0; i < FRAME_SIZE; i++) {
                                combined[i] += co->inbuf.back();
                                co->inbuf.pop_back();
                            }
                        }
                        lck.unlock();
                    }
                }
                //if (c->inbuf.size() >= FRAME_SIZE * 4) {
                //    for (int i = 0; i < c->inbuf.size() - FRAME_SIZE * 4; i++) {
                //        c->inbuf.pop_back();
                //    }
                //    printf("Popping\n");
                //}

                uint8_t cbits[MAX_PACKET_SIZE + 1];
                cbits[0] = REQ_ADATA;
                int nBytes = opus_encode(c->enc, combined, FRAME_SIZE, cbits + 1, MAX_PACKET_SIZE);
                if (nBytes < 0) {
                    fprintf(stderr, "encode failed: %s\n", opus_strerror(nBytes));
                    exit(1);
                }                
                socket_.send_to(asio::buffer(cbits, nBytes + 1), c->endpt);
                idx++;
            }
            unsigned long long endns = std::chrono::duration_cast< std::chrono::microseconds >(
	    	        std::chrono::system_clock::now().time_since_epoch()).count();
	    	std::this_thread::sleep_for(std::chrono::microseconds(((FRAME_SIZE * 1000000) / SAMPLE_RATE) - (endns - startns) - 50));
        }
        opus_encoder_destroy(enc);
    }
private:
    udp::socket socket_;
    udp::endpoint remote_endpoint_;
    unsigned char cbytes[MAX_PACKET_SIZE + 1];
    OpusEncoder *enc;
    std::deque<opus_int16> outbuf;
    std::mutex outbuf_m;
    std::vector<Connection*> conns;
};

void start_server() {
    try {
        asio::io_context ctx;
        udp_server server(ctx);
        std::thread thr([&ctx]() { ctx.run(); });
        server.calculate_shit();
        thr.join();
    } catch (std::exception &e) {
        fprintf(stderr, "server error: %s\n", e.what());
    }
}

int main() {
    start_server();
    

    return 0;
}