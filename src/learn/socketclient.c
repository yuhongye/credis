#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = htons(9734);
    int len = sizeof(address);

    int client_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int result = connect(client_sockfd, (struct sockaddr *) &address, len);
    if (result == -1) {
        perror("oops: client2");
        exit(1);
    }
    
    char ch = 'A';
    write(client_sockfd, &ch, 1);
    read(client_sockfd, &ch, 1);
    printf("char from server = %c\n", ch);
    close(client_sockfd);

    exit(0);
}