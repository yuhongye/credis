#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <netinet/in.h>

void processConnect(int server_sockfd, fd_set *readfds) {
    struct sockaddr_in client_address;
    unsigned int client_len = sizeof(client_address);
    int client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_address, &client_len);
    // 将client加入到集合中，当有数据是会被select?
    FD_SET(client_sockfd, readfds);
    printf("adding client on fd: %d\n", client_sockfd);
}

void processRequest(int fd, fd_set *readfds) {
    int nread;
    char ch;
    // client有数据请求
    ioctl(fd, FIONREAD, &nread);
    if (nread == 0) {
        close(fd);
        FD_CLR(fd, readfds);
        printf("removing client on fd %d\n", fd);
    } else {
        read(fd, &ch, 1);
        sleep(5);
        printf("serving client on fd %d\n", fd);
        ch++;
        write(fd, &ch, 1);
    }

}

int main() {
    // 建立服务器端socket
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(9734);
    int server_len = sizeof(server_address);

    int server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    bind(server_sockfd, (struct sockaddr *) &server_address, server_len);
    listen(server_sockfd, 5);

    fd_set readfds;
    FD_ZERO(&readfds);
    // 将服务器端socket加入到集合中
    FD_SET(server_sockfd, &readfds);

    while (1) {
        char ch;
        printf("server waiting\n");

        int result = select(FD_SETSIZE, &readfds, (fd_set *)0, (fd_set *) 0, (struct timeval *)0);
        if (result < 1) {
            perror("sever 5");
            exit(1);
        }

        // 扫描所有文件描述符
        for (int fd = 0; fd < FD_SETSIZE; fd++) {
            if (FD_ISSET(fd, &readfds)) {
                // 判断是否为服务器套接字，是则表示为客户请求连接
                if (fd == server_sockfd) {
                    processConnect(server_sockfd, &readfds);
                } else {
                    processRequest(fd, &readfds);
                }
            }
        }
    }
}