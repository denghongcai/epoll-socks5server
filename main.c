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

#define DEBUG_INFO 3
#define DEBUG_WARN 2
#define DEBUG_ERR  1

#define DEBUG_LEVEL DEBUG_ERR // Set debug mode and debug level

#if defined(DEBUG) && DEBUG_LEVEL > 0
 #define DEBUG_PRINT(level, fmt, args...) do { \
     if(level <= DEBUG_LEVEL) { \
         fprintf(stderr, "DEBUG: %s:%d:%s(): " fmt, \
             __FILE__, __LINE__, __func__, ##args); \
     } \
 } while(0)
#else
 #define DEBUG_PRINT(fmt, args...) /* Don't do anything in release builds */
#endif

#define MAX_EVENTS 200
#define PORT 130

struct event_data // Used by epoll_data_t.ptr
{
    int type; // 0 for client, 1 for connect cmd
    int state;
    int writestate;
    int fd;
    void* pairptr;
    int pairclosed;
    char* sendbuf;
    int sendbuflen;
    char* recvbuf;
    int recvbuflen;
};

// {{{ Function: setnonblocking
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
// }}}

// {{{ Function: hostname_to_ip
int hostname_to_ip(char *hostname , char *ip)
{
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_in *h;
    int rv;
 
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // use AF_INET6 to force IPv6
    hints.ai_socktype = SOCK_STREAM;
 
    if ( (rv = getaddrinfo( hostname , "http" , &hints , &servinfo)) != 0) 
    {
        DEBUG_PRINT(DEBUG_ERR, "getaddrinfo: %s\n", gai_strerror(rv));
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
// }}}

// {{{ Function: truncatemem
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
// }}}

// {{{ Function: rmfromepoll
void rmfromepoll(int epfd, struct event_data* data_ptr)
{
    if(data_ptr->fd == 0) {
        return;
    }
    DEBUG_PRINT(DEBUG_INFO, "disconnected, type: %d\n", data_ptr->type);
    if(data_ptr->sendbuf != NULL) {
        free(data_ptr->sendbuf);
    }
    if(data_ptr->recvbuf != NULL) {
        free(data_ptr->recvbuf);
    }
    epoll_ctl(epfd, EPOLL_CTL_DEL, data_ptr->fd, NULL);
    if(close(data_ptr->fd) != 0) {
        perror("close");
    }
    data_ptr->fd = 0;
    if(data_ptr->pairptr != NULL && ((struct event_data*)data_ptr->pairptr)->sendbuf == NULL) {
        rmfromepoll(epfd, data_ptr->pairptr);
    }
    if(data_ptr->pairptr != NULL && ((struct event_data*)data_ptr->pairptr)->sendbuf != NULL) {
        ((struct event_data*)data_ptr->pairptr)->pairclosed = 1;
        ((struct event_data*)data_ptr->pairptr)->pairptr = NULL;
    }
    free(data_ptr);
}
// }}}

int recvdata(int epfd, struct event_data* data_ptr)
{
    char** recvbuf = (char**)(&(data_ptr->recvbuf));
    int* recvbuflen = (int*)(&(data_ptr->recvbuflen));
    char tmpbuf[BUFSIZ];
    char* buf = NULL;
    int n = 0, nread = 0;
    while ((nread = read(data_ptr->fd, tmpbuf, BUFSIZ)) > 0) {
        buf = (char*)realloc(buf, (n + nread)*sizeof(char));
        memcpy(buf + n, tmpbuf, nread * sizeof(char));
        n += nread;
    }

    if (nread == -1 && errno != EAGAIN) {
        if(buf != NULL) {
            free(buf);
        }
        perror("read error");
        rmfromepoll(epfd, data_ptr);
        return -1;
    }

    DEBUG_PRINT(DEBUG_INFO, "recvlen: %d, type: %d\n", n, data_ptr->type);

    (*recvbuf) = (char*)realloc(*recvbuf, (*recvbuflen + n)*sizeof(char)); 
    memcpy((*recvbuf) + (*recvbuflen), buf, n * sizeof(char));
    (*recvbuflen) = *recvbuflen + n;

    if(buf != NULL) {
        free(buf);
    }

    if (nread == 0) {
        return 1;
    }
    else{ 
        return 0;
    }
}

int senddata(int epfd, struct event_data* data_ptr)
{
    if( data_ptr->writestate == 0 ) {
        if((data_ptr->sendbuf) != NULL) {
            DEBUG_PRINT(DEBUG_INFO, "sendlen: %d\n", data_ptr->sendbuflen);
            int nwrite, data_size = data_ptr->sendbuflen;
            int n = data_ptr->sendbuflen;
            while (n > 0) {
                nwrite = write(data_ptr->fd, data_ptr->sendbuf + data_size - n, n);
                if (nwrite < n) {
                    if (nwrite == -1 && errno != EAGAIN) {
                        perror("write error");
                    }
                    if ( errno == EAGAIN ) {
                        if( nwrite != -1 ) {
                            n -= nwrite;
                        }
                        struct epoll_event ev;
                        ev.data.ptr = data_ptr;
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLET; // Add listener for EPOLLOUT
                        if (epoll_ctl(epfd, EPOLL_CTL_MOD, data_ptr->fd, &ev) == -1) {
                            perror("epoll_ctl: mod");
                        }

                        data_ptr->writestate = 1;
                        DEBUG_PRINT(DEBUG_INFO, "EAGAIN, %d\n", data_ptr->type);
                    }
                    break;
                }
                n -= nwrite;
            }
            DEBUG_PRINT(DEBUG_INFO, "sentlen: %d\n", data_ptr->sendbuflen - n);
            if(n == 0){
                free(data_ptr->sendbuf);
                data_ptr->sendbuf = NULL;
                data_ptr->sendbuflen = 0;
                if(data_ptr->pairclosed == 1) {
                    rmfromepoll(epfd, data_ptr);
                }
            }
            else {
                char* tmp = (char*)malloc(n * sizeof(char)); 
                memcpy(tmp, data_ptr->sendbuf + (data_ptr->sendbuflen - n), n); 
                free(data_ptr->sendbuf);
                data_ptr->sendbuf = tmp;
                data_ptr->sendbuflen = n;
            }
        }
    }
     
    return 0;
}

// {{{ Fucntion: Socks5StateMachineClient
int Socks5StateMachineClient(int epfd, struct event_data* data_ptr)
{
    int* state = &(data_ptr->state);
    char** recvbuf = &(data_ptr->recvbuf);
    int* recvbuflen = &(data_ptr->recvbuflen);
    char** sendbuf = &(data_ptr->sendbuf);
    int* sendbuflen = &(data_ptr->sendbuflen);
    if(*state == 0)
    {
        if(*recvbuflen >= 3) {
            truncatemem(recvbuf, recvbuflen, 2 + (int)(unsigned int)((unsigned char)(*recvbuf)[1]));
            char response[2];
            response[0] = 0x05;
            response[1] = 0x00;

            *sendbuf = realloc(*sendbuf, 2*sizeof(char));
            memcpy(*sendbuf, response, 2); // Response of Socks5 protocal handshake ( no validation )
            *sendbuflen = 2;
            *state = 1;
            DEBUG_PRINT(DEBUG_INFO, "handshake\n");
        } 
        else {
            *state = -1;
        }
    }
    else if(*state == 1)
    {
        char response = 0x07;
        char ipaddress[20] = {0};
        uint16_t port;
        DEBUG_PRINT(DEBUG_INFO, "decode request recvbuflen %d\n", *recvbuflen);
        if(*recvbuflen > 4) {
            DEBUG_PRINT(DEBUG_INFO, "decode request atyp %d\n", (unsigned int)(*recvbuf)[3]);
            switch((unsigned int)(*recvbuf)[3])
            {
                case 0x01: // IPv4
                    if(*recvbuflen > 8) {
                        snprintf(ipaddress, 20, "%u.%u.%u.%u", (unsigned int)((unsigned char)(*recvbuf)[4]), (unsigned int)((unsigned char)(*recvbuf)[5]), (unsigned int)((unsigned char)(*recvbuf)[6]), (unsigned int)((unsigned char)(*recvbuf)[7])); // pay attention to range of unsigned char
                        truncatemem(recvbuf, recvbuflen, 8); 
                        port = ntohs(*(uint16_t*)(*recvbuf)); // net order to host order
                        truncatemem(recvbuf, recvbuflen, 2); 
                    }
                    else {
                        *state = -1;
                    }
                    break;
                case 0x03: // Domain
                    {
                        int offset = 0;
                        char domainlen = (*recvbuf)[4];
                        char* domain = (char*)malloc(((unsigned int)(unsigned char)domainlen)*sizeof(char) + 1);
                        memset(domain, 0, ((unsigned int)(unsigned char)domainlen)*sizeof(char) + 1);
                        memcpy(domain, *recvbuf + 5, (unsigned int)domainlen);
                        hostname_to_ip(domain, ipaddress);
                        DEBUG_PRINT(DEBUG_INFO, "domain: %s\n", ipaddress);
                        free(domain); 
                        offset += 1 + (unsigned int)domainlen;
                        truncatemem(recvbuf, recvbuflen, 4 + offset);
                        port = ntohs(*(uint16_t*)(*recvbuf)); // net order to host order
                        truncatemem(recvbuf, recvbuflen, 2); 
                    }
                    break;
                case 0x04:
                    break;
                default:
                    response = 0x08;
                    break;
            }
        }
        else {
            *state = -1;
        }
        if(strnlen(ipaddress, 8) != 0 && response == 0x07) {
            DEBUG_PRINT(DEBUG_INFO, "request %s:%u\n", ipaddress, port); 
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
            // Prepare for connect
            int connectfd = socket(AF_INET, SOCK_STREAM, 0);
            if(connectfd < 0) {
                perror("connect");
                *state = -1;
            } 
            struct sockaddr_in addr;  
            bzero(&addr, sizeof(addr));  
            addr.sin_family = AF_INET;  
            addr.sin_port = htons(port);  
            addr.sin_addr.s_addr = inet_addr(ipaddress);  
            setnonblocking(connectfd);

            connect(connectfd, (const struct sockaddr*)&addr, sizeof(addr));

            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDHUP | EPOLLET;
            ev.data.ptr = malloc(sizeof(struct event_data));
            memset(ev.data.ptr, 0, sizeof(struct event_data)); // May cause strange problem. http://stackoverflow.com/questions/11152160/initializing-a-struct-to-0
            ((struct event_data*)ev.data.ptr)->fd = connectfd;
            ((struct event_data*)ev.data.ptr)->type = 1;
            ((struct event_data*)ev.data.ptr)->pairptr = data_ptr;
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, connectfd,
                        &ev) == -1) {
                perror("epoll_ctl: add");
                exit(EXIT_FAILURE);
            }
            data_ptr->pairptr = ev.data.ptr;

            *state = 2;
        }
        else {
            *state = -1;
        }
    }
    else if(*state == 2) {
        struct event_data* pairptr = (struct event_data*)(data_ptr->pairptr);
        pairptr->sendbuf = (char*)realloc(pairptr->sendbuf, (pairptr->sendbuflen + data_ptr->recvbuflen) * sizeof(char));
        memcpy(pairptr->sendbuf + pairptr->sendbuflen, data_ptr->recvbuf, data_ptr->recvbuflen);
        pairptr->sendbuflen += data_ptr->recvbuflen;

        free(data_ptr->recvbuf);
        data_ptr->recvbuf = NULL;
        data_ptr->recvbuflen = 0;
        senddata(epfd, pairptr);
    }
    else if(*state == -1) {
        return 1;
    }
    return 0;
}
// }}}

int main(){
    struct epoll_event ev, events[MAX_EVENTS];
    int addrlen, listenfd, conn_sock, nfds, epfd, fd, i;
    int so_reuseaddr = 1;
    struct event_data* data_ptr;
    struct sockaddr_in local, remote;

    if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("sockfd");
        exit(1);
    }
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr));
    setnonblocking(listenfd);
    bzero(&local, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);;
    local.sin_port = htons(PORT);
    if( bind(listenfd, (struct sockaddr *) &local, sizeof(local)) < 0) {
        perror("bind");
        exit(1);
    }

    if(listen(listenfd, 100) < 0) {
        perror("listen");
        exit(1);
    }

    epfd = epoll_create(MAX_EVENTS); // Create epoll fd for accept client
    if (epfd == -1) {
        perror("epoll_create");
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN;
    ev.data.ptr = malloc(sizeof(struct event_data));
    ((struct event_data*)ev.data.ptr)->fd = listenfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    for (;;) {
        // {{{ Accept events process
        nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }

        for (i = 0; i < nfds; ++i) {
            fd = ((struct event_data*)events[i].data.ptr)->fd;
            data_ptr = (struct event_data*)events[i].data.ptr;
            if (fd == listenfd) {
                while ((conn_sock = accept(listenfd,(struct sockaddr *) &remote, 
                                (socklen_t *)&addrlen)) > 0) {
                    setnonblocking(conn_sock);
                    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLET;
                    ev.data.ptr = malloc(sizeof(struct event_data));
                    memset(ev.data.ptr, 0, sizeof(struct event_data)); // May cause strange problem. http://stackoverflow.com/questions/11152160/initializing-a-struct-to-0
                    ((struct event_data*)ev.data.ptr)->fd = conn_sock;
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
                int error = recvdata(epfd, data_ptr);
                if(error < 0) {
                    continue;
                }
                if(data_ptr->type == 0) {
                    if(Socks5StateMachineClient(epfd, data_ptr) != 0) {
                        rmfromepoll(epfd, data_ptr);
                        continue;
                    }
                    senddata(epfd, data_ptr);
                }
                else if(data_ptr->type == 1) {
                    struct event_data* pairptr = (struct event_data*)(data_ptr->pairptr);
                    pairptr->sendbuf = (char*)realloc(pairptr->sendbuf, (pairptr->sendbuflen + data_ptr->recvbuflen) * sizeof(char));
                    memcpy(pairptr->sendbuf + pairptr->sendbuflen, data_ptr->recvbuf, data_ptr->recvbuflen * sizeof(char));
                    pairptr->sendbuflen += data_ptr->recvbuflen;

                    free(data_ptr->recvbuf);
                    data_ptr->recvbuf = NULL;
                    data_ptr->recvbuflen = 0;
                    
                    senddata(epfd, pairptr);
                }
                if(error == 1) {
                    rmfromepoll(epfd, data_ptr);
                }
            }
            if (events[i].events & EPOLLOUT) {
                DEBUG_PRINT(DEBUG_INFO, "EPOLLOUT %d\n", data_ptr->type);
                ev.data.ptr = data_ptr;
                ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLET; // Remove listener for EPOLLOUT
                if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                    perror("epoll_ctl: mod");
                }

                data_ptr->writestate = 0;
                senddata(epfd, data_ptr);
            }
            if ((events[i].events & EPOLLRDHUP) || (events[i].events & EPOLLERR)) {
                rmfromepoll(epfd, data_ptr);
            }
        }
        // }}}
    }
    return 0;
}
