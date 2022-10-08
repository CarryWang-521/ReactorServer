#include "Server.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>

typedef struct ThreadParam {
    pthread_t tid;
    int fd;
    int epfd;
} ThreadParam;

int InitListenFd(unsigned short port)
{
    // 创建监听fd
    int lfd = socket(AF_INET, SOCK_STREAM, 0); // 0代表: 使用流式套接字的tcp
    if (lfd == -1) {
        perror("socket");
        return -1;
    }
    // 设置端口复用（服务器“主动”断开连接后，需要等待2msl的时长才可以释放端口，在此期间如需访问该端口，则需要设置端口复用）
    int opt = 1;
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    if (ret == -1) {
        perror("setsockopt");
        return -1;
    }
    // 绑定
    struct sockaddr_in sAddr;
    sAddr.sin_family = AF_INET;
    sAddr.sin_port = htons(port);
    sAddr.sin_addr.s_addr = INADDR_ANY;
    ret = bind(lfd, (struct sockaddr*)&sAddr, sizeof sAddr);
    if (ret == -1) {
        perror("bind");
        return -1;
    }
    // 设置监听
    ret = listen(lfd, 128);
    if (ret == -1) {
        perror("listen");
        return -1;
    }

    return lfd;
}

int EpollRun(int lfd)
{
    // 创建epoll实例
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create");
        return -1;
    }
    // lfd上树
    struct epoll_event ev;
    ev.data.fd = lfd;
    ev.events = EPOLLIN;
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return -1;
    }
    // 检测
    struct epoll_event evs[1024];
    int size = sizeof(evs) / sizeof(evs[0]);
    while (1) {
        int num = epoll_wait(epfd, evs, size, -1); // -1阻塞等待事件
        for (int i = 0; i < num; ++i) {
            int fd = evs[i].data.fd;
            ThreadParam* info = (ThreadParam*)malloc(sizeof(ThreadParam));
            info->epfd = epfd;
            info->fd = fd;
            if (fd == lfd) {
                // 建立连接
                pthread_create(&info->tid, NULL, AcceptClient, info);
            } else {
                // 主要接受对端数据http
                pthread_create(&info->tid, NULL, RecvHttpRequest, info);
            }
        }
    }
    return 0;
}

void* AcceptClient(void* arg)
{
    ThreadParam* info = (ThreadParam*)arg;
    int cfd = accept(info->fd, NULL, NULL);
    if (cfd == -1) {
        perror("accept");
        return NULL;
    }
    // 设置非阻塞(防止连接的客户端没有数据发送时的read阻塞)
    int flag = fcntl(cfd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);
    // 添加到epoll
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET; // 设置水平模式->边缘模式
    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        close(cfd);
    }
    return NULL;
}
void* RecvHttpRequest(void* arg)
{
    printf("start recv http request\r\n");
    ThreadParam* info = (ThreadParam*)arg;
    char buf[4096] = { 0 };
    char tmp[1024] = { 0 };
    long len = 0;
    long total = 0;
    /*
    recv被设置为非阻塞，所以数据读完后还会继续读数据
    recv读不到数据返回-1，读取失败返回-1
    区分的方法时不同的perror
    */
    while ((len = recv(info->fd, tmp, sizeof tmp, 0)) > 0) {
        // 仅确保请求行可以读取到，后面get数据可以忽略
        if (total + len < sizeof buf) {
            memcpy(buf + total, tmp, (size_t)len);
        }
        total += len;
    }
    // 判断数据是否接受完毕
    if (len == -1 && errno == EAGAIN) {
        // 解析请求行
        char* pt = strstr(buf, "\r\n");
        long reqLen = pt - buf;
        buf[reqLen] = '\0';
        ParseHttpRequest(buf, info->fd);
    } else if (len == 0) {
        // 客户端断开连接
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
    } else {
        perror("recv");
    }
    close(info->fd);

    return NULL;
}

int ParseHttpRequest(const char* line, int cfd)
{
    // 解析请求行
    char method[12] = { 0 };
    char path[1024] = { 0 };
    sscanf(line, "%[^ ] %[^ ]", method, path);
    printf("method: %s, path: %s\r\n", method, path);
    if (strcasecmp(method, "get") != 0) {
        return -1;
    }
    // 将客户端的特殊符号由utf-8转至特殊字符
    DecodeHttpMsg(path, path);
    printf("method: %s, path: %s\r\n", method, path);

    // 处理客户端请求的静态资源(目录或资源)
    char* file = NULL;
    if (strcmp(path, "/") == 0) {
        file = "./";
    } else {
        file = path + 1; // 将/去掉
    }
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1) {
        // 文件不存在 -- 回复404
        SendHttpResponseHead(cfd, 404, "Not Found", GetFileType(".html"), -1);
        SendFile("404.html", cfd);
        return 0;
    }
    // 判断文件类型
    if (S_ISDIR(st.st_mode)) {
        // 把目录内容发送给客户端
        SendHttpResponseHead(cfd, 200, "OK", GetFileType(".html"), -1);
        SendDir(file, cfd);
    } else {
        // 把文件内容发送给客户端
        SendHttpResponseHead(cfd, 200, "OK", GetFileType(file), st.st_size);
        SendFile(file, cfd);
    }

    return 0;
}

int SendHttpResponseHead(int cfd, int status, const char* desc, const char* type, long length)
{
    // 状态行
    char buf[4096] = { 0 };
    sprintf(buf, "http/1.1 %d %s\r\n", status, desc);

    // 响应头
    sprintf(buf + strlen(buf), "content-type: %s\r\n", type);
    sprintf(buf + strlen(buf), "content-length: %d\r\n\r\n", length);

    send(cfd, buf, strlen(buf), 0);

    return 0;
}

int SendFile(const char* fileName, int cfd)
{
    // 读一部分，发一部分，tcp流式协议
    int fd = open(fileName, O_RDONLY);
    assert(fd > 0);
#if 0
    while (1) {
        char buf[1024];
        int len = read(fd, buf, sizeof buf);
        if (len > 0) {
            send(cfd, buf, len, 0);
            usleep(10); // 非常重要，需要给客户端接收/解析数据的时间
        } else if(len == 0) {
            break;
        } else {
            perror("read");
        }
    }
#else
    long size = lseek(fd, 0, SEEK_END); // 或者使用stat
    lseek(fd, 0, SEEK_SET);
    off_t offset = 0;
    while (offset < size) {
        printf("offset: %ld, totalSize:%ld\r\n", offset, size);
        ssize_t ret = sendfile(cfd, fd, &offset, (size_t)(size - offset)); // ***由于cfd被设置为非阻塞模式，所以send数据时，从cfd的write缓冲区读取数据，会存在fd中的数据还没有被内核写入到cfd的write的缓冲区，这时候发数据就会报临时文件不可用
        printf("send size: %ld\r\n", ret);
        if (ret == -1 && errno == EAGAIN) {
            perror("sendfile");
            printf("cfd 对应的 write缓冲区还没有数据。。。\n");
        }
    }

#endif
    close(fd);
    return 0;
}

int SendDir(const char* dirName, int cfd)
{
    char buf[4096] = { 0 };
    sprintf(buf, "<html><head><title><%s></title></head><body><table>", dirName);
    // nameList指向 struct dirent* tmp[];
    struct dirent** nameList;
    int num = scandir(dirName, &nameList, NULL, alphasort);
    for (int i = 0; i < num; ++i) {
        // 取出文件名
        char* name = nameList[i]->d_name;
        struct stat st;
        char subPath[1024] = { 0 };
        sprintf(subPath, "%s/%s", dirName, name);
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode)) {
            // a标签<a href=""></a>>
            sprintf(buf + strlen(buf), "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        } else {
            sprintf(buf + strlen(buf), "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        }
        send(cfd, buf, strlen(buf), 0);
        memset(buf, 0, sizeof(buf));

        free(nameList[i]);
    }
    sprintf(buf, "</table></body></html>");
    send(cfd, buf, strlen(buf), 0);
    free(nameList);
    return 0;
}

const char* GetFileType(const char* fileName)
{
    const char* dot = strrchr(fileName, '.'); // 从右向左找.

    if (dot == NULL) {
        return "text/plain; charset=utf-8"; // 纯文本
    }
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".css") == 0) {
        return "text/css";
    }
    if (strcasecmp(dot, ".au") == 0) {
        return "audio/basic";
    }
    if (strcasecmp(dot, ".au") == 0) {
        return "audio/basic";
    }
    if (strcasecmp(dot, ".wav") == 0) {
        return "audio/wav";
    }
    if (strcasecmp(dot, ".avi") == 0) {
        return "audio/x-msvideo";
    }

    return "text/plain; charset=utf-8";
}

void DecodeHttpMsg(char* to, char* from)
{
    for (; *from != '\0'; ++to, ++from) {
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            // 将16进制的数字 --> 十进制数值
            *to = HexToDec(from[1]) * 16 + HexToDec(from[2]);
            from += 2;
        } else {
            *to = *from;
        }
    }
    *to = *from;
}

int HexToDec(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0;
}
