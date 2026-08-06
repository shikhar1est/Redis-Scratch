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
#include <poll.h>
#include <string>
#include <vector>
#include <map>
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

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) {
        return false;
    }
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool
read_str(const uint8_t *&cur, const uint8_t *end, size_t n, string &out) {
    if (cur + n > end) {
        return false;
    }
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

static int32_t
parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out) {
    const uint8_t *end = data + size;
    uint32_t nstr = 0;
    if (!read_u32(data, end, nstr)) {
        return -1;
    }
    if (nstr > MAX_REQUEST_SIZE) {
        return -1;  // safety limit
    }

    while (out.size() < nstr) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) {
            return -1;
        }
        out.push_back(std::string());
        if (!read_str(data, end, len, out.back())) {
            return -1;
        }
    }
    if (data != end) {
        return -1;  // trailing garbage
    }
    return 0;
}

// Response::status
enum {
    RES_OK = 0,
    RES_ERR = 1,    // error
    RES_NX = 2,     // key not found
};

struct Response {
    uint32_t status = 0;
    std::vector<uint8_t> data;
};

static std::map<std::string, std::string> g_data;

static void do_request(std::vector<std::string> &cmd, Response &out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        auto it = g_data.find(cmd[1]);
        if (it == g_data.end()) {
            out.status = RES_NX;    // not found
            return;
        }
        const std::string &val = it->second;
        out.data.assign(val.begin(), val.end());
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        g_data[cmd[1]].swap(cmd[2]);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        g_data.erase(cmd[1]);
    } else {
        out.status = RES_ERR;       // unrecognized command
    }
}

static void make_response(const Response &resp, std::vector<uint8_t> &out) {
    uint32_t resp_len = 4 + (uint32_t)resp.data.size();
    buf_append(out, (const uint8_t *)&resp_len, 4);
    buf_append(out, (const uint8_t *)&resp.status, 4);
    buf_append(out, resp.data.data(), resp.data.size());
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
    if (4+len> conn->incoming.size()) {
        return false;   // want read
    }
    const uint8_t *request = &conn->incoming[4];

    std::vector<std::string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        msg("bad request");
        conn->want_close = true;
        return false;   // want close
    }
    Response resp;
    do_request(cmd, resp);
    make_response(resp, conn->outgoing);

    // application logic done! remove the request message.
    buf_consume(conn->incoming, 4 + len);
    // Q: Why not just empty the buffer? See the explanation of "pipelining".
    return true;        // success
}


//This function handles the writing of data from the outgoing buffer of a connection represented by the Conn structure.
static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
    if (rv < 0 && errno == EAGAIN) {
        return; // actually not ready
    }
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;    // error handling
        return;
    }

    // remove written data from `outgoing`
    buf_consume(conn->outgoing, (size_t)rv);

    // update the readiness intention
    if (conn->outgoing.size() == 0) {   // all data written
        conn->want_read = true;
        conn->want_write = false;
    } // else: want write
}

//This function handles the reading of data from a connection represented by the Conn structure.
static void handle_read(Conn *conn) {
    // read some data
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0 && errno == EAGAIN) {
        return; // actually not ready
    }
    // handle IO error
    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return; // want close
    }
    // handle EOF
    if (rv == 0) {
        if (conn->incoming.size() == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return; // want close
    }
    // got some new data
    buf_append(conn->incoming, buf, (size_t)rv);

    // parse requests and generate responses
    while (try_one_request(conn)) {}
    // Q: Why calling this in a loop? See the explanation of "pipelining".

    // update the readiness intention
    if (conn->outgoing.size() > 0) {    // has a response
        conn->want_read = false;
        conn->want_write = true;
        // The socket is likely ready to write in a request-response protocol,
        // try to write it without waiting for the next iteration.
        return handle_write(conn);
    }   // else: want read
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

fd_set_nb(fd); //making the listening socket non-blocking using the fd_set_nb() function defined earlier.

//LISTEN
rv=listen(fd,SOMAXCONN); //This line puts the socket into listening mode using the listen() function.
// The second argument SOMAXCONN specifies the maximum number of pending connections that can be queued
if(rv){
    die("listen failed");
}

// a map of all client connections, keyed by fd
    std::vector<Conn *> fd2conn;
    // the event loop
    std::vector<struct pollfd> poll_args;
    while (true) {
        // prepare the arguments of the poll()
        poll_args.clear();
        // put the listening sockets in the first position
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        // the rest are connection sockets
        for (Conn *conn : fd2conn) {
            if (!conn) {
                continue;
            }
            // always poll() for error
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            // poll() flags from the application's intent
            if (conn->want_read) {
                pfd.events |= POLLIN;
            }
            if (conn->want_write) {
                pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        // wait for readiness
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR) {
            continue;   // not an error
        }
        if (rv < 0) {
            die("poll");
        }

        // handle the listening socket
        if (poll_args[0].revents) {
            if (Conn *conn = handle_accept(fd)) {
                // put it into the map
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        // handle connection sockets
        for (size_t i = 1; i < poll_args.size(); ++i) { // note: skip the 1st
            uint32_t ready = poll_args[i].revents;
            if (ready == 0) {
                continue;
            }

            Conn *conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN) {
                assert(conn->want_read);
                handle_read(conn);  // application logic
            }
            if (ready & POLLOUT) {
                assert(conn->want_write);
                handle_write(conn); // application logic
            }

            // close the socket from socket error or application logic
            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }
        }   // for each connection sockets
    }   // the event loop
    return 0;
}




//In Redis,a command is represented as a list of strings, where the first string is the command name and the subsequent strings are the command arguments.

//Serialization means converting the command into a format that can be transmitted over the network, while 
//deserialization means converting the received data back into a command format that can be processed by the server.

//Length prefixing is a technique used in Redis to indicate the length of each string in the command.
//An outer length prefix is used to indicate the number of strings in the command, while 
//an inner length prefix is used to indicate the length of each individual string.
//An nstr field is used to indicate the number of strings in the command, while a len field is used to indicate the length of each individual string.

//Length-prefixing is robust because it allows the server to know exactly how many bytes to read for each string, 
//even if the string contains null bytes or other special characters.

//Upto now try_one_request() function is implemented to handle a single request from the client. It was basically read one full message from client,
//process it, and generate a response. However, in a real-world scenario, clients may send multiple requests in a single connection, 
//and the server should be able to handle them efficiently. This is where pipelining comes into play.
