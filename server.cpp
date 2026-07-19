#include <sys/socket.h> //This header file contains definitions of structures needed for sockets, e.g. sockaddr
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void do_something(int connfd) { 
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
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
    struct sockaddr_in client_addr={}; //This line declares a variable client_addr of type struct sockaddr_in to store the address information of the incoming client connection.
    socklen_t client_addr_len=sizeof(client_addr); //This line declares a variable client_addr_len of type socklen_t and initializes it to the size of the client_addr structure.
    //socklen_t is a data type used to store the size of a socket address structure (such as struct sockaddr_in).
    int connfd=accept(fd,(struct sockaddr*)&client_addr,&client_addr_len); //This line accepts an incoming connection using the accept() function.
    if(connfd<0){
        continue; //If the accept() call fails, it returns -1 and sets errno to indicate the error. In this case, the code 
        //simply continues to the next iteration of the loop, allowing the server to keep running and accepting new connections.
    }
    do_something(connfd); //This line calls a function do_something() to handle the accepted connection.
    close(connfd); //This line closes the accepted connection using the close() function, releasing the resources associated with it.
}

}