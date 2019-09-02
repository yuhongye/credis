#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main() {
    char buf[128];
    fd_set inputs, testfds;
    FD_ZERO(&inputs);
    // 把要检测的句柄stdin(0)加入到集合中
    FD_SET(0, &inputs);

    struct timeval timeout;
    int result;
    int nread;
    while (1) {
        testfds = inputs;
        timeout.tv_sec = 5;
        timeout.tv_usec = 500 * 1000;

        result = select(FD_SETSIZE, &testfds, (fd_set *)0, (fd_set *)0, &timeout);
        switch (result){
        case 0:
            printf("timeout\n");
            break;
        case -1:
            perror("select");
            exit(1);
        default:
            if (FD_ISSET(0, &testfds)) {
                // 获取从键盘输入的字符数，包括回车
                ioctl(0, FIONREAD, &nread);
                if (nread == 1) {
                    printf("keyboard done\n");
                    exit(0);
                }

                nread = read(0, buf, nread);
                buf[nread] = '\0';
                printf("read %d from keyboard: %s", nread, buf);
            }
            break;
        }
    }
    return 0;
}