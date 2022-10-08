#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "Server.h"

int main(int argc, char* argv[])
{
    if (argc < 3) {
        printf("Input param error, e.g.: ./a.out port path\n");
        return -1;
    }
    unsigned short port = (unsigned short)atoi(argv[1]);
    // 切换服务器的工作路径
    chdir(argv[2]);
    // 初始化用于监听的套接字
    int lfd = InitListenFd(port);
    if (lfd == -1) {
        return 0;
    }
    // 启动监听服务
    EpollRun(lfd);

    return 0;
}