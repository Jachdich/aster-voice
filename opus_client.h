#ifndef __VOICE_CLIENT_H
#define __VOICE_CLIENT_H
using udp = asio::ip::udp;
#define FRAME_SIZE 960
#define SAMPLE_RATE 48000
#define APPLICATION OPUS_APPLICATION_AUDIO
#define BITRATE 64000

#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE (3*1276)

struct SoundIo;

class VoiceClient {
public:
    udp::socket sock;
    udp::endpoint endp;
    OpusEncoder *enc;
    OpusDecoder *dec;
    
    std::deque<int16_t> inbuf;
    std::deque<int16_t> outbuf;
    std::mutex inm;
    std::mutex outm;
    std::condition_variable condvar;
    struct SoundIo *soundio;
    unsigned char netbuf[MAX_PACKET_SIZE + 1];
    bool stopped = false;
    bool send_ident = true;
    bool send_end = false;
    
    VoiceClient(asio::io_context &ctx);
    void start_recv();
    void handle_recv(const asio::error_code &ec, size_t nBytes);
    void soundio_run();
    void stop();
    void run(uint64_t uuid);
};

#endif