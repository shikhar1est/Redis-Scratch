#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>


static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static int32_t read_full(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error, or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static void
buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

const size_t k_max_msg = 32<<20;  // 32MB

// the `query` function was simply splited into `send_req` and `read_res`.
static int32_t send_req(int fd, const uint8_t *text, size_t len) { //This function 
    if (len > k_max_msg) {
        return -1;
    }

    std::vector<uint8_t> wbuf;
    buf_append(wbuf, (const uint8_t *)&len, 4);
    buf_append(wbuf, text, len);
    return write_all(fd, wbuf.data(), wbuf.size());
}

static int32_t read_res(int fd) {
    // 4 bytes header
    std::vector<uint8_t> rbuf;
    rbuf.resize(4);
    errno = 0;
    int32_t err = read_full(fd, &rbuf[0], 4);
    if (err) {
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf.data(), 4);  // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    // reply body
    rbuf.resize(4 + len);
    err = read_full(fd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // do something
    printf("len:%u data:%.*s\n", len, len < 100 ? len : 100, &rbuf[4]);
    return 0;
}


int main(){
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){
        die("socket failed");
    }
    struct sockaddr_in addr={}; //here we are creating a sockaddr_in structure to hold the address of the server we want to connect to. 
    //The sockaddr_in structure is used for IPv4 addresses and contains fields for the address family, port number, and IP address.
    addr.sin_family=AF_INET;
    addr.sin_port=ntohs(1234); //ntohs() and htons() are used to convert between host byte order and network byte order for 16-bit values.
    addr.sin_addr.s_addr=ntohl(INADDR_LOOPBACK); //ntohl() and htonl() are used to convert between host byte order and network byte order for 32-bit values.
    int rv=connect(fd,(struct sockaddr*)&addr,sizeof(addr)); //This line attempts to establish a connection to the server using the connect() function.
    if(rv){
        die("connect failed");
    }
     // multiple pipelined requests
    std::vector<std::string> query_list = {
        "hello1", "hello2", "hello3",
        // a large message requires multiple event loop iterations
        std::string(k_max_msg, 'z'),
        "hello5",
    };
    for (const std::string &s : query_list) {
        int32_t err = send_req(fd, (uint8_t *)s.data(), s.size());
        if (err) {
            goto L_DONE;
        }
    }
    for (size_t i = 0; i < query_list.size(); ++i) {
        int32_t err = read_res(fd);
        if (err) {
            goto L_DONE;
        }
    }

L_DONE:
    close(fd);
    return 0;
}


//poll() is a system call that allows a program to monitor multiple file descriptors 
//(such as sockets) for events, such as incoming data or the ability to write data.
// It is commonly used in network programming to handle multiple connections simultaneously without blocking the program's execution.

//TCP is a stream of bytes, not a stream of messages. This means that when you send data over a TCP connection, the data is sent 
//as a continuous stream of bytes, rather than as discrete messages.


//In the current setup(event loop 6a), we have one server and multiple clients. 
//The server is responsible for handling incoming connections and processing requests from the clients. 
//Each client can send multiple requests to the server, and the server will respond to each request in the order they were received. 
//This setup allows for efficient communication between the server and multiple clients, enabling concurrent processing of requests.

//Imagine a single connection between a client and a server. There's a bug inside each individual connection. 
//The bug is that the server is not able to handle multiple requests from the client in a single connection. 
//It asssumes 1 read event, 1 request and 1 response. This assumption fails when the client sends multiple requests in a single connection, and the server should be able 
//to handle that.
//The TCP mindset is not "I got one message", It's "I got a steam of bytes, let's see how many messages I can parse from it". 
//The server should be able to handle multiple requests in a single connection, and the client should be able to send multiple 
//requests in a single connection as well.
//When it comes to response, the server should be able to send multiple responses in a single connection as well.
//Multiple responses accumulate in the socket buffer, and the client should be able to read multiple responses in a single connection as well.

//handle_write() is a function that is responsible for handling write events on a socket. Responsible for sending data from the server to the client. 
//When the server has data to send, it will call handle_write() to write the data to the socket.
//handle_read() is a function that is responsible for handling read events on a socket. Responsible for receiving data from the client to the server.