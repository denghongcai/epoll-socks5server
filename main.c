#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <errno.h> 

#define MAX_EVENTS 10
#define PORT 130

void setnonblocking(int sockfd) {
    int opts;

    opts = fcntl(sockfd, F_GETFL);
    if(opts < 0) {
        perror("fcntl(F_GETFL)\n");
        exit(1);
    }
    opts = (opts | O_NONBLOCK);
    if(fcntl(sockfd, F_SETFL, opts) < 0) {
        perror("fcntl(F_SETFL)\n");
        exit(1);
    }
}

int hostname_to_ip(char *hostname , char *ip)
{
    int sockfd;  
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_in *h;
    int rv;
 
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // use AF_INET6 to force IPv6
    hints.ai_socktype = SOCK_STREAM;
 
    if ( (rv = getaddrinfo( hostname , "http" , &hints , &servinfo)) != 0) 
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }
 
    // loop through all the results and connect to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) 
    {
        h = (struct sockaddr_in *) p->ai_addr;
        strcpy(ip , inet_ntoa( h->sin_addr ) );
    }
     
    freeaddrinfo(servinfo); // all done with this structure
    return 0;
}

void truncatemem(char** recvbuf, int* recvbuflen, int offset)
{
    char* tmp;
    int newsize = *recvbuflen - offset;
    tmp = (char*)malloc(newsize*sizeof(char));
    memcpy(tmp, *recvbuf + offset, newsize);
    free(*recvbuf);
    *recvbuf = tmp; 
    *recvbuflen = newsize;
}

int Socks5StateMachineClient(int* state, int i, char** recvbuf, int* recvbuflen, char** sendbuf, int* sendbuflen)
{
        if(*state == 0)
        {
            if(*recvbuflen >= 3) {
                truncatemem(recvbuf, recvbuflen, 3);
                char response[2];
                response[0] = 0x05;
                response[1] = 0x00;

                *sendbuf = realloc(*sendbuf, 2*sizeof(char));
                memcpy(*sendbuf, response, 2); // Response of Socks5 protocal handshake ( no validation )
                *sendbuflen = 2;
                *state = 1;
                printf("handshake\n");
            } 
        }
        else if(*state == 1)
        {
            char response = 0x07;
            char ipaddress[20] = {0};
            uint16_t port;
            printf("decode request recvbuflen %d\n", *recvbuflen);
            if(*recvbuflen > 4) {
                printf("decode request atyp %d\n", (unsigned int)(*recvbuf)[3]);
                switch((unsigned int)(*recvbuf)[3])
                {
                    case 0x01: // IPv4
                        if(*recvbuflen > 8) {
                            snprintf(ipaddress, 20, "%u.%u.%u.%u", (unsigned int)((unsigned char)(*recvbuf)[4]), (unsigned int)((unsigned char)(*recvbuf)[5]), (unsigned int)((unsigned char)(*recvbuf)[6]), (unsigned int)((unsigned char)(*recvbuf)[7])); // pay attention to range of unsigned char
                            truncatemem(recvbuf, recvbuflen, 8); 
                            port = ntohs(*(uint16_t*)(*recvbuf)); // net order to host order
                            truncatemem(recvbuf, recvbuflen, 2); 
                        }
                        break;
                    case 0x03: // Domain, not implement yet
                        {
                            int offset = 0;
                            char domainlen = (*recvbuf)[4];
                            char* domain = (char*)malloc(((unsigned int)(unsigned char)domainlen)*sizeof(char));
                            memcpy(domain, *recvbuf + 5, (unsigned int)domainlen);
                            hostname_to_ip(domain, ipaddress);
                            free(domain); 
                            offset += 1 + (unsigned int)domainlen;
                            truncatemem(recvbuf, recvbuflen, 4 + offset);
                        }
                        break;
                    case 0x04:
                        break;
                    default:
                        response = 0x08;
                        break;
                }
            }
            if(strnlen(ipaddress, 8) != 0 && response == 0x07) {
                printf("request %s:%u\n", ipaddress, port); 
                response = 0x00;
                *sendbuf = (char*)malloc(10 * sizeof(char));
                (*sendbuf)[0] = 0x05;
                (*sendbuf)[1] = response;
                (*sendbuf)[2] = 0x00;
                (*sendbuf)[3] = 0x01; // Always ipv4
                (*sendbuf)[4] = 0x00;
                (*sendbuf)[5] = 0x00;
                (*sendbuf)[6] = 0x00;
                (*sendbuf)[7] = 0x00;
                (*sendbuf)[8] = 0x00;
                (*sendbuf)[9] = 0x00;
                *sendbuflen = 10;
                *state = 2;
            }
        }
        else if(*state == 2) {
            printf("%s\n", *recvbuf);
            return 1;
        }
        return 0;
}

int main(){
    struct epoll_event ev, events[MAX_EVENTS];
    int addrlen, listenfd, conn_sock, nfds, epfd, fd, i, nread, n;
    int flag = 1, len = sizeof(int);
    struct sockaddr_in local, remote;
    char buf[BUFSIZ];
    char* clientrecvbuf[MAX_EVENTS] = {NULL};
    char* clientsendbuf[MAX_EVENTS] = {NULL};
    char* exchangeptr;
    int clientstatus[MAX_EVENTS] = {0};
    int clientsendbuflen[MAX_EVENTS] = {0};
    int clientrecvbuflen[MAX_EVENTS] = {0};
    int clientrequestfd[MAX_EVENTS] = {0};
    int requestclientfd[MAX_EVENTS] = {0};

    if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("sockfd\n");
        exit(1);
    }
    setnonblocking(listenfd);
    bzero(&local, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);;
    local.sin_port = htons(PORT);
    if( bind(listenfd, (struct sockaddr *) &local, sizeof(local)) < 0) {
        perror("bind\n");
        exit(1);
    }
    listen(listenfd, 20);

    epfd = epoll_create(MAX_EVENTS);
    if (epfd == -1) {
        perror("epoll_create");
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN;
    ev.data.fd = listenfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    for (;;) {
        nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }

        for (i = 0; i < nfds; ++i) {
            fd = events[i].data.fd;
            if (fd == listenfd) {
                while ((conn_sock = accept(listenfd,(struct sockaddr *) &remote, 
                                (socklen_t *)&addrlen)) > 0) {
                    setnonblocking(conn_sock);
                    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
                    ev.data.fd = conn_sock;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_sock,
                                &ev) == -1) {
                        perror("epoll_ctl: add");
                        exit(EXIT_FAILURE);
                    }
                }
                if (conn_sock == -1) {
                    if (errno != EAGAIN && errno != ECONNABORTED 
                            && errno != EPROTO && errno != EINTR) 
                        perror("accept");
                }
                continue;
            }  
            if (events[i].events & EPOLLIN) {
                n = 0;
                while ((nread = read(fd, buf + n, BUFSIZ-1)) > 0) {
                    n += nread;
                }
                if (nread == -1 && errno != EAGAIN) {
                    perror("read error");
                }
                if (nread == 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    if(close(fd) != 0) {
                        perror("close");
                    }
                    continue;
                }
                clientrecvbuf[i] = (char*)realloc(clientrecvbuf[i], clientrecvbuflen[i] + n*sizeof(char)); 
                memcpy(clientrecvbuf[i] + clientrecvbuflen[i], buf, n);
                clientrecvbuflen[i] = clientrecvbuflen[i] + n;
                printf("state 1  %d\n", clientstatus[i]);
                
                if(Socks5StateMachineClient(&clientstatus[i], i, &clientrecvbuf[i], &clientrecvbuflen[i], &clientsendbuf[i], &clientsendbuflen[i]) != 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    if(close(fd) != 0) {
                        perror("close");
                    }
                    continue;
                }
                printf("state 2  %d\n", clientstatus[i]);

                ev.data.fd = fd;
                ev.events = events[i].events | EPOLLOUT; // Listen for EPOLLOUT
                if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                    perror("epoll_ctl: mod");
                }
            }
            if (events[i].events & EPOLLOUT) {
                if(clientsendbuf[i] != NULL) {
                    //Socks5StateMachineOUT(&clientstatus[i], i, &clientbuf[i], &clientbuflen[i]);
                    int nwrite, data_size = clientsendbuflen[i];
                    n = clientsendbuflen[i];
                    while (n > 0) {
                        nwrite = write(fd, clientsendbuf[i] + data_size - n, n);
                        if (nwrite < n) {
                            if (nwrite == -1 && errno != EAGAIN) {
                                perror("write error");
                            }
                            break;
                        }
                        n -= nwrite;
                    }
                    free(clientsendbuf[i]);
                    clientsendbuf[i] = NULL;
                    clientsendbuflen[i] = 0;
                }
            }
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                if(close(fd) != 0) {
                    perror("close");
                }
            }
        }
    }

    return 0;
}
