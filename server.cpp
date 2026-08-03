#include <sys/socket.h> //This header file contains definitions of structures needed for sockets, e.g. sockaddr
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <stdbool.h>
#include <vector>
#include <fcntl.h>
using namespace std;

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char *msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

const size_t MAX_REQUEST_SIZE = 32<<20; //This line defines a constant MAX_REQUEST_SIZE with a value of 32 shifted left by 20 bits,
// which is equivalent to 32 * 2^20 = 33,554,432 bytes (32 MB).

struct Conn{ //Object to represent a connection. It contains the following members:
    int fd=-1; 
    bool want_read=false;
    bool want_write=false;
    bool want_close=false;
    std::vector<uint8_t> incoming; //buffered incoming data
    std::vector<uint8_t> outgoing; //buffered outgoing data

};

static void fd_set_nb(int fd){ //This function sets the file descriptor fd to non-blocking mode using the fcntl() system call.
    errno=0;
    int flags=fcntl(fd,F_GETFL,0); //This line retrieves the current file
    if(errno){
        die("fcntl error");
        return;
    }
    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }

}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) { //This function appends data to a buffer 
    //represented by a std::vector<uint8_t> called buf.
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n) { //THis function removes the first n bytes from the
    // buffer represented by a std::vector<uint8_t> called buf.
    buf.erase(buf.begin(), buf.begin() + n);
}

static Conn *handle_accept(int fd) { //This function handles the acceptance of a new connection on the listening socket
    // represented by the file descriptor fd.
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        msg_errno("accept() error");
        return NULL;
    }
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port)
    );

    // set the new connection fd to nonblocking mode
    fd_set_nb(connfd);

    // create a `struct Conn`
    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

static bool try_one_request(Conn *conn) {
    // try to parse the protocol: message header
    if (conn->incoming.size() < 4) {
        return false;   // want read
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > MAX_REQUEST_SIZE) {
        msg("too long");
        conn->want_close = true;
        return false;   // want close
    }
    // message body
    if (4 + len > conn->incoming.size()) {
        return false;   // want read
    }
    const uint8_t *request = &conn->incoming[4];

    // got one request, do some application logic
    printf("client says: len:%d data:%.*s\n",
        len, len < 100 ? len : 100, request);

    // generate the response (echo)
    buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    buf_append(conn->outgoing, request, len);

    // application logic done! remove the request message.
    buf_consume(conn->incoming, 4 + len);
    // Q: Why not just empty the buffer? See the explanation of "pipelining".
    return true;        // success
}

static int32_t read_all(int fd,char *buf,size_t size){ //This function reads data from a file descriptor (fd) into a buffer (buf) until the specified size (size) is reached. It returns 0 on success and -1 on failure.
    while(size>0){
        ssize_t rv=read(fd,buf,size);
        if(rv<=0){
            return -1;
        }
        assert((size_t)rv<=size); //asseert() is used to check that the number of bytes read (rv) is less than or equal to the
        // remaining size to be read (size). If this condition is false, the program will terminate with an assertion failure.
        size-=(ssize_t)rv;
        buf+=rv;
    }
    return 0;
}

static int32_t write_all(int fd,char *buf,size_t size){
    while(size>0){
        ssize_t rv=write(fd,buf,size);
        if(rv<=0){
            return -1;
        }
        assert((size_t)rv<=size);
        size-=(ssize_t)rv;
        buf+=rv;
    }
    return 0;
}

//This function is the protocol parser and handler for a single request. 
//It reads the request from the connection represented by connfd, processes it, and sends a reply back to the client.
static int32_t one_request(int connfd) { 
    // 4 bytes header
    char rbuf[4 + MAX_REQUEST_SIZE];
    errno = 0;
    int32_t err = read_all(connfd, rbuf, 4);
    if (err) {
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);  // memcpy() is used to copy the first 4 bytes from the rbuf buffer into the len variable.
    // This is done to extract the length of the request body from the received data.
    if (len > MAX_REQUEST_SIZE) {
        msg("too long");
        return -1;
    }

    // request body
    err = read_all(connfd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // do something
    fprintf(stderr, "client says: %.*s\n", len, &rbuf[4]);

    // reply using the same protocol
    const char reply[] = "world";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_all(connfd, wbuf, 4 + len);
}
static void do_something(int connfd) { //This function handles the accepted connection represented by the file descriptor connfd.
    char rbuf[64] = {}; //A buffer rbuf of size 64 bytes is declared and initialized to zero. This buffer will be used to store data read from the client.
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1); //read() function is called to read data from the accepted connection (connfd) into the rbuf buffer. 
    //The size of the buffer is specified as sizeof(rbuf) - 1 to leave space for a null terminator. read() returns the number of bytes read, or -1 if an error occurs. The result is stored in the variable n.
    if (n < 0) {
        msg("read() error");
        return;
    }
    fprintf(stderr, "client says: %s\n", rbuf);

    char wbuf[] = "world";
    write(connfd, wbuf, strlen(wbuf));
}



int main(){

    //CREATING A SOCKET
int fd=socket(AF_INET,SOCK_STREAM,0); //Creating a socket using the socket() function. 
//Value returned by socket() is a file descriptor for the new socket and an integer value that cannot be negative. 
// If the socket creation fails, it returns -1 and sets errno to indicate the error.
if(fd<0){
    die("socket failed");
}

//CONFIGURING SOCKET OPTIONS
int val=1;
setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&val,sizeof(val));
//This line sets the socket option SO_REUSEADDR to allow the socket to bind to an address that is already in use.
//  This is useful for quickly restarting a server without waiting for the socket to be released.
//Without it, the OS may prevent the server from binding to the same address and port if it was recently used.
//While waiting for some TCP packets to be released, the socket may remain in a TIME_WAIT state, preventing immediate reuse of the same address and port combination.

//BINDING THE SOCKET TO AN ADDRESS AND PORT
struct sockaddr_in addr={}; //This line declares a variable addr of type struct sockaddr_in and initializes it to zero.
//The struct sockaddr_in is used to store Ip address and port information
addr.sin_family=AF_INET; //This line sets the sin_family field of the addr structure to AF_INET, indicating that the socket will use the IPv4 address family.
addr.sin_port=htons(1234); //This line sets the sin_port field of the addr structure to the port number 1234,
// converted to network byte order using the htons() function.
addr.sin_addr.s_addr=htonl(0); //This line sets the sin_addr.s_addr field of the addr structure to the IP address
int rv=bind(fd,(struct sockaddr*)&addr,sizeof(addr)); //This line binds the socket to the specified address and port using the bind() function.
if(rv<0){
    die("bind failed");
}

//LISTEN
rv=listen(fd,SOMAXCONN); //This line puts the socket into listening mode using the listen() function.
// The second argument SOMAXCONN specifies the maximum number of pending connections that can be queued
if(rv){
    die("listen failed");
}

//ACCEPT CONNECTIONS
while(true){
    struct sockaddr_in caddr={};
    socklen_t caddrlen=sizeof(caddr);
    int connfd=accept(fd,(struct sockaddr*)&caddr,&caddrlen); 
    if(connfd<0){
        die("accept failed");
    }
    while(true){
        int32_t err=one_request(connfd);
        if(err){
            break;
        }
    }
    close(connfd);
}


return 0;
}