#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>


static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
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
    char msg[]="hello brother";
    write(fd,msg,sizeof(msg));
    char rbuf[64]={};
    ssize_t n=read(fd,rbuf,sizeof(rbuf)-1);
    if(n<0){
        die("read failed");
    }
    printf("server says: %s\n", rbuf);
    close(fd);
}