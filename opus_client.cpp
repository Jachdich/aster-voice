#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <opus/opus.h>
#include <stdio.h>
#include <soundio/soundio.h>
#include <math.h>
#include <asio.hpp>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <thread>
#include <chrono>
#include "opus_client.h"

enum {
    REQ_ACK,
    REQ_ADATA,
    REQ_IDENT,
    REQ_END
};

void read_callback(struct SoundIoInStream *instream, int frame_count_min, int frame_count_max) {
    struct SoundIoChannelArea *areas;
    int err;
    VoiceClient *client = (VoiceClient*)instream->userdata;
    
    int frames_left = frame_count_max;
    for (;;) {
        int frame_count = frames_left;
        if ((err = soundio_instream_begin_read(instream, &areas, &frame_count))) {
            fprintf(stderr, "begin read error: %s", soundio_strerror(err));
            exit(1);
        }
        //printf("Reading %d frames\n", frame_count);
        if (!frame_count)
            break;
        std::unique_lock<std::mutex> lock(client->inm);
        if (!areas) {
            // Due to an overflow there is a hole. Fill the ring buffer with
            // silence for the size of the hole.
            //memset(buffer + buf_end, 0, frame_count * instream->bytes_per_frame);
            //buf_end += frame_count;
            fprintf(stderr, "Dropped %d frames due to internal overflow\n", frame_count);
        } else {
            for (int frame = 0; frame < frame_count; frame += 1) {
                int16_t val;
                for (int ch = 0; ch < instream->layout.channel_count; ch += 1) {
                    val = *areas[ch].ptr;
                    areas[ch].ptr += areas[ch].step;
                }
                client->inbuf.push_back(val);
                if (client->inbuf.size() > FRAME_SIZE) {
                    client->condvar.notify_all();
                }
            }
        }
        lock.unlock();
        if ((err = soundio_instream_end_read(instream))) {
            fprintf(stderr, "end read error: %s", soundio_strerror(err));
            exit(0);
        }
        frames_left -= frame_count;
        if (frames_left <= 0)
            break;
    }
}

void write_callback(struct SoundIoOutStream *outstream,
                    int frame_count_min, int frame_count_max) {
    VoiceClient *client = (VoiceClient*)outstream->userdata;
    
    struct SoundIoChannelArea *areas;
    std::lock_guard<std::mutex> lock(client->outm);
    //int frames_left = min(frame_count_max, buffer.size());
    int frames_left = frame_count_max;
    //if (frame_count_min > frames_left || frames_left == 0) { 
    //    fprintf(stderr, "Buffer underflow #2 %d\n", buffer.size());
    //}
    //printf("%d %d %d\n", frame_count_min, frame_count_max, client->outbuf.size());
    int err;

    while (frames_left > 0) {
        int frame_count = frames_left;
        if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
            fprintf(stderr, "%s\n", soundio_strerror(err));
            exit(1);
        }

        if (!frame_count) break;
        int done = false;
        for (int frame = 0; frame < frame_count; frame += 1) {
            int16_t val = 0;
            if (client->outbuf.size() <= 1920 && !done) {
                printf("Buffer underflow!\n");
                done = true;
            }
            if (client->outbuf.size() > 1920) {
                val = client->outbuf.back();
                client->outbuf.pop_back();

                //uint8_t *v = (uint8_t*)&val;
                //uint8_t temp = v[0];
                //v[0] = v[1];
                //v[1] = temp;
                //printf("%d\n", val);
            }
            for (int channel = 0; channel < outstream->layout.channel_count; channel += 1) {
                int16_t *ptr = (int16_t*)(areas[channel].ptr + areas[channel].step * frame);
                *ptr = val;
            }
            
        }

        if ((err = soundio_outstream_end_write(outstream))) {
            fprintf(stderr, "%s\n", soundio_strerror(err));
            exit(1);
        }
        printf("Consumed %d frames\n", frame_count);
        frames_left -= frame_count;
    }
}

void setup_opus(OpusEncoder **enc, OpusDecoder **dec) {
    int err;
    if (enc != NULL) {
        *enc = opus_encoder_create(48000, 1, APPLICATION, &err);
        if (err < 0) {
            fprintf(stderr, "failed to create encoder: %s\n", opus_strerror(err));
            exit(1);
        }

        err = opus_encoder_ctl(*enc, OPUS_SET_BITRATE(BITRATE));
        if (err < 0) {
            fprintf(stderr, "failed to set bitrate: %s\n", opus_strerror(err));
            exit(1);
        }
    }

    if (dec != NULL) {
        *dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
        if (err < 0) {
            fprintf(stderr, "failed to create decoder: %s\n", opus_strerror(err));
            exit(1);
        }
    }
}

void VoiceClient::soundio_run() {
    soundio = soundio_create();
    
    int err;
    if ((err = soundio_connect(soundio))) {
        fprintf(stderr, "failed to initialise soundio: %s\n", soundio_strerror(err));
        exit(1);
    }
    soundio_flush_events(soundio);

    int default_in_device_index = soundio_default_input_device_index(soundio);
    struct SoundIoDevice *indevice = soundio_get_input_device(soundio, default_in_device_index);
    struct SoundIoInStream *instream = soundio_instream_create(indevice);

    instream->format = SoundIoFormatS16NE;
    instream->read_callback = read_callback;
    instream->userdata = this;
    instream->software_latency = 0.1;
    
    if ((err = soundio_instream_open(instream))) {
        fprintf(stderr, "unable to open device: %s", soundio_strerror(err));
        exit(1);
    }
    
    if (instream->layout_error)
        fprintf(stderr, "unable to set channel layout: %s\n", soundio_strerror(instream->layout_error));
    
    if ((err = soundio_instream_start(instream))) {
        fprintf(stderr, "unable to start device: %s\n", soundio_strerror(err));
        exit(1);
    }

    int default_out_device_index = soundio_default_output_device_index(soundio);
    struct SoundIoDevice *outdevice = soundio_get_output_device(soundio, default_out_device_index);
    struct SoundIoOutStream *outstream = soundio_outstream_create(outdevice);

    outstream->format = SoundIoFormatS16NE;
    outstream->write_callback = write_callback;
    outstream->userdata = this;
    outstream->software_latency = 0.1;
    
    if ((err = soundio_outstream_open(outstream))) {
        fprintf(stderr, "unable to open device: %s", soundio_strerror(err));
        exit(1);
    }
    
    if (outstream->layout_error)
        fprintf(stderr, "unable to set channel layout: %s\n", soundio_strerror(outstream->layout_error));
    
    if ((err = soundio_outstream_start(outstream))) {
        fprintf(stderr, "unable to start device: %s\n", soundio_strerror(err));
        exit(1);
    }
    
    while (!stopped)
        soundio_wait_events(soundio);
    
    soundio_instream_destroy(instream);
    soundio_outstream_destroy(outstream);
    soundio_device_unref(indevice);
    soundio_device_unref(outdevice);
    soundio_destroy(soundio);
}

void VoiceClient::start_recv() {
    sock.async_receive_from(
        asio::buffer(netbuf, MAX_FRAME_SIZE), endp,
        [this](const asio::error_code &ec, size_t bytes) {
            this->handle_recv(ec, bytes);
        }
    );
}

void VoiceClient::handle_recv(const asio::error_code &ec, size_t nBytes) {
    if (ec) {
        fprintf(stderr, "ERROR: %s\n", ec.message().c_str());
    } else {
        printf("Recvd %d bytes\n", nBytes);

        uint8_t req = netbuf[0];
        if (req == REQ_ACK) {
            printf("Got ack\n");
            if (send_ident) {
                send_ident = false;
            } else if (send_end) {
                send_end = false;
            } else {
                printf("Spurious ack\n");
            }
        } else if (req == REQ_ADATA) {
            
            opus_int16 out[MAX_FRAME_SIZE];
            int frame_size = opus_decode(dec, netbuf + 1, nBytes - 1, out, MAX_FRAME_SIZE, 0);
            if (frame_size < 0) {
                fprintf(stderr, "decoder failed: %s\n", opus_strerror(frame_size));
                exit(1);
            }
            
            std::lock_guard<std::mutex> lock(outm);
            for (int i = 0; i < frame_size; i++) {
                outbuf.push_front(out[i]);
            }
        } else {
            printf("Got spurious request of type %d\n", req);
        }
        
        if (!stopped || send_end) {
            start_recv();
        }
    }
}

VoiceClient::VoiceClient(asio::io_context &ctx) : sock(ctx) {
    udp::resolver resolver(ctx);
    udp::resolver::query query(udp::v4(), "127.0.0.1", "2346"); 
    endp = *resolver.resolve(query).begin();
    sock.open(udp::v4());
}

void VoiceClient::run(uint64_t uuid) {
    opus_int16 in[FRAME_SIZE];

    unsigned char cbits[MAX_PACKET_SIZE];
    int nBytes;
    //setup_opus(&enc, &dec);

    while (!stopped || send_end) {
        printf("%d %d %d\n", stopped, send_ident, send_end);
        if (send_ident) {
            uint8_t data[9];
            data[0] = REQ_IDENT;
            memcpy(data + 1, &uuid, 8);
            sock.send_to(asio::buffer(data, 9), endp);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else if (send_end) {
            uint8_t data[1] = {REQ_END};
            printf("Sending end\n");
            sock.send_to(asio::buffer(data, 1), endp);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else {
            std::unique_lock<std::mutex> lck(inm);
            while (inbuf.size() < FRAME_SIZE && !stopped) condvar.wait(lck);
            if (!stopped) {
                for (int i = 0; i < FRAME_SIZE; i++) {
                    in[i] = inbuf.front();
                    inbuf.pop_front();
                }
                lck.unlock();
                nBytes = opus_encode(enc, in, FRAME_SIZE, cbits, MAX_PACKET_SIZE);
                if (nBytes < 0) {
                    fprintf(stderr, "encode failed: %s\n", opus_strerror(nBytes));
                    exit(1);
                }

                uint8_t *sendbuf = (uint8_t*)malloc(nBytes + 1);
                sendbuf[0] = REQ_ADATA;
                memcpy(sendbuf + 1, cbits, nBytes);

                sock.send_to(asio::buffer(sendbuf, nBytes + 1), endp);
            }
        }
    }
    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
}

void VoiceClient::stop() {
    stopped = true;
    send_end = true;
    soundio_wakeup(soundio);
    condvar.notify_all();
}

#include <iostream>
int main() {
    try {
        asio::io_context ctx;
        VoiceClient client(ctx);
        setup_opus(&client.enc, &client.dec);
        client.start_recv();
        std::thread clientthread([&client]() { client.run(69420); });
        std::thread netthr([&ctx]() { ctx.run(); });
        std::thread soundthr([&client]() { client.soundio_run(); });
        std::string s;
        std::cin >> s;
        printf("Shutting down..\n");
        client.stop();
        soundthr.join();
        netthr.join();
        clientthread.join();
    } catch (std::exception &e) {
        fprintf(stderr, "Client error: %s\n", e.what());
    }
}