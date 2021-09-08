#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <asio.hpp>
#include <deque>
#include <mutex>
#include <iostream>
#include <thread>
#define MAX_PACKET_SIZE (3*1276)

using udp = asio::ip::udp;
using tcp = asio::ip::tcp;

enum {
    REQ_ACK,
    REQ_ADATA,
    REQ_IDENT,
    REQ_END
};

struct Connection {
    udp::endpoint endpt;
    uint64_t uuid;
    uint64_t iters_without_msg;
    Connection(udp::endpoint endp) {
        endpt = endp;
        uuid = 0;
        iters_without_msg = 0;
    }
};

class UdpServer;

class TcpClient {
private:
    asio::streambuf buf;
    asio::io_context ctx;
    tcp::socket socket;
    std::thread asioThread;
    UdpServer *parent;
public:
    TcpClient(UdpServer *parent) : socket(ctx) {
        this->parent = parent;
    }
    std::error_code connect(std::string address, uint16_t port);
    std::error_code sendRequest(std::string request);
    void handler(std::error_code ec, size_t bytes_transferred);
    void readUntil();
};

class UdpServer {
public:
    UdpServer(asio::io_context &ctx) : socket_(ctx, udp::endpoint(udp::v4(), 2346)), client(this) {
        start_recv();
        std::error_code ec = client.connect("127.0.0.1", 5432);
        if (ec) {
            std::cout << "Error connecting to text server: " << ec << "\n";
        }
    }
    void handle_tcp_msg(std::string data) {
        std::cout << "Got reply: " << data << "\n";
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
                std::error_code err = client.sendRequest("{\"command\": \"join\", \"uuid\": " + std::to_string(conn->uuid) + "}");
                if (err) {
                    std::cout << "Sending req error: " << err.message() << "\n";
                }
            } else if (req == REQ_END) {
                if (conn->uuid != 0) {
                    delete conn;
                    conns.erase(std::next(conns.begin(), idx));
                } else {
                    printf("END req without identifying first\n");
                }
                uint8_t ack_msg[1] = {REQ_ACK};
                socket_.send_to(asio::buffer(ack_msg, 1), remote_endpoint_);
            } else if (req == REQ_ADATA) {
                if (conn->uuid != 0) {
                    conn->iters_without_msg = 0;
                    uint8_t senddata[MAX_PACKET_SIZE + 9];
                    senddata[0] = REQ_ADATA;
                    memcpy(senddata + 1, &conn->uuid, 8);
                    memcpy(senddata + 9, cbytes + 1, nBytes - 1);
                    sendall(asio::buffer(senddata, nBytes + 8), conn);
                } else {
                    printf("ADATA req without identifying first\n");
                }
            }
            start_recv();
        }
    }

    void sendall(const asio::const_buffer &buf, Connection *ignore) {
        for (Connection *conn : conns) {
            if (conn == ignore) continue;
            socket_.send_to(buf, conn->endpt);
        }
    }
private:
    udp::socket socket_;
    udp::endpoint remote_endpoint_;
    unsigned char cbytes[MAX_PACKET_SIZE + 1];
    std::mutex outbuf_m;
    std::vector<Connection*> conns;
    TcpClient client;
};

std::error_code TcpClient::connect(std::string address, uint16_t port) {
    asio::error_code ec;

    asio::ip::tcp::resolver resolver(ctx);
    auto endpoint = resolver.resolve(address, std::to_string(port), ec);
    if (ec) return ec;
    asio::connect(socket, endpoint, ec);
    if (ec) return ec;

    readUntil();
    asioThread = std::thread([&]() {ctx.run();});
    return ec;
}

std::error_code TcpClient::sendRequest(std::string request) {
    asio::error_code error;

    asio::write(socket, asio::buffer(request + "\n"), error);
    return error;
}

void TcpClient::handler(std::error_code ec, size_t bytes_transferred) {
    std::cout << bytes_transferred << "\n";
    if (!ec) {
        std::string data(
                buffers_begin(buf.data()),
                buffers_begin(buf.data()) + (bytes_transferred
                  - 1));
                  
        buf.consume(bytes_transferred);
        readUntil();

        parent->handle_tcp_msg(data);

    } else {
        std::cerr << "TCP ERROR: " <<  ec.message() << "\n";
        readUntil();
    }
}

void TcpClient::readUntil() {
    asio::async_read_until(socket, buf, '\n', [this] (std::error_code ec, std::size_t bytes_transferred) {
        handler(ec, bytes_transferred);
    });
}

void start_server() {
    //try {
        asio::io_context ctx;
        UdpServer server(ctx);
        ctx.run();
    //} catch (std::exception &e) {
    //    fprintf(stderr, "server error: %s\n", e.what());
    //}
}

int main() {
    start_server();
    

    return 0;
}
