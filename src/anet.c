#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "anet.h"

static void anetSetError(char *err, const char *fmt, ...) {
    if (err == NULL) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

/** set tcp options */

/**
 * 将fd设置为NONBLOCK
 */
int anetNonBlock(char *err, int fd) {
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s\n", strerror(errno));
        return ANET_ERR;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        anetSetError(err, "fcntl(F_SETFL, O_NONBLOCK): %s\n", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetTcpNoDelay(char *err, int fd) {
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt TCP_NODELAY: %s\n", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetSetSendBuffer(char *err, int fd, int bufferSize) {
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) == -1) {
        anetSetError(err, "setsockopt, SO_SNDBUF: %s\n", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetTcpKeepAlive(char *err, int fd) {
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s\n", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/**
 * 解析host的ip地址放到ipbuf中
 */ 
int anetResolve(char *err, char *host, char *ipbuf) {
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    // 将十进制点分ip转成网络字节序ip, 返回0说明失败
    if (inet_aton(host, &sa.sin_addr) == 0) {
        struct hostent *he;

        he = gethostbyname(host);
        if (he == NULL) {
            anetSetError(err, "can't resolve: %s\n", host);
            return ANET_ERR;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
    }

    //            网络序ip转换成十进制ip, 这里为什么不用host? 是因为host有可能是网址吗？
    strcpy(ipbuf, inet_ntoa(sa.sin_addr));
    return ANET_OK;
}

#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1

/**
 * @return socket的句柄，如果发生了错误则返回ANET_ERR
 */
static int anetTcpGenericConnect(char *err, char *addr, int port, int flags) {
    int s;
    if (s = socket(AF_INET, SOCK_STREAM, 0) == -1) {
        anetSetError(err, "creating socket: %s\n", strerror(errno));
        return ANET_ERR;
    }
    int on = 1;
    /** 连接密集型的任务(比如redis benchmark)可以进行大量的close/open操作 */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // 解析地址
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_aton(addr, &sa.sin_addr) == 0) {
        struct hostent  *he;
        he = gethostbyname(addr);
        if (he == NULL) {
            anetSetError(err, "can't resolve: %s\n", addr);
            close(s);
            return ANET_ERR;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
    }

    // 如果是nonblock，设置标志位
    if ((flags & ANET_CONNECT_NONBLOCK) && (anetNonBlock(err, s) != ANET_OK)) {
        return ANET_ERR;
    }

    // 连接
    if (connect(s, (struct sockaddr *) &sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK) {
            return s;
        }

        anetSetError(err, "connect: %s\n", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
}

int anetTcpConnect(char *err, char *addr, int port) {
    return anetTcpGenericConnect(err, addr, port, ANET_CONNECT_NONE);
}

int anetTcpNonBlockConnect(char *err, char *addr, int port) {
    return anetTcpGenericConnect(err, addr, port, ANET_CONNECT_NONBLOCK);
}

/**
 * @return total byte read from fd, or -1 if read() return -1
 */
int anetRead(int fd, void *buf, int count) {
    int nread, totlen = 0;
    while (totlen != count) {
        nread = read(fd, buf, count-totlen);
        if (nread == 0) {
            return totlen;
        }
        if (nread == -1) {
            return -1;
        }
        totlen += nread;
        buf += nread;
    }
    return totlen;
}

/**
 * @return total bytes write to fd, or -1 if write() return -1
 */
int anetWrite(int fd, void *buf, int count) {
    int nwriten, totlen = 0;
    while (totlen != count) {
        nwriten = write(fd, buf, count-totlen);
        if (nwriten == 0) {
            return totlen;
        }
        if (nwriten == -1) {
            return -1;
        }
        totlen += nwriten;
        buf += nwriten;
    }
    return totlen;
}

int anetTcpServer(char *err, int port, char *bindaddr) {
    int s;
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        anetSetError(err, "socket: %s\n", strerror(errno));
        return ANET_ERR;
    }

    int on = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        anetSetError(err, "setsockopt, SO_REUSEADDR: %s\n", strerror(errno));
        close(s);
        return ANET_ERR;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = port;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bindaddr != NULL) {
        if (inet_aton(bindaddr, &sa.sin_addr) == 0) {
            anetSetError(err, "Invalid bind address\n");
            close(s);
            return ANET_ERR;
        }
    }

    if (bind(s, (struct sockaddr*) &sa, sizeof(sa)) == -1) {
        anetSetError(err, "bind: %s\n", strerror(errno));
        close(s);
        return ANET_ERR;
    }

    /**
     * listen可以让套接字进入被动监听状态，也就是当没有client请求时，socket处于“睡眠”状态，
     * 只有当接受到客户端请求时，socket才被唤醒来响应请求
     * 32指的是请求队列的长度
     */
    if (listen(s, 32) == -1) {
        anetSetError(err, "listen: %s\n", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
}

/**
 * @param ip: accept获取的socket的ip
 * @param port: accept获取的socket的port
 * @return 连接的套接字
 */
int anetAccept(char *err, int serversock, char *ip, int *port) {
    int fd;
    struct sockaddr_in sa;
    unsigned int salen;

    while (1) {
        salen = sizeof(sa);
        fd = accept(serversock, (struct sockaddr *) &sa, &salen);
        if (fd == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                anetSetError(err, "accept: %s\n", strerror(errno));
                return ANET_ERR;
            }
        }
        break;
    }

    if (ip != NULL) {
        strcpy(ip, inet_ntoa(sa.sin_addr));
    }
    if (port != NULL) {
        *port = ntohs(sa.sin_port);
    }
    return fd;
}
