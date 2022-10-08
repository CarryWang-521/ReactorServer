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
    // ��������fd
    int lfd = socket(AF_INET, SOCK_STREAM, 0); // 0����: ʹ����ʽ�׽��ֵ�tcp
    if (lfd == -1) {
        perror("socket");
        return -1;
    }
    // ���ö˿ڸ��ã����������������Ͽ����Ӻ���Ҫ�ȴ�2msl��ʱ���ſ����ͷŶ˿ڣ��ڴ��ڼ�������ʸö˿ڣ�����Ҫ���ö˿ڸ��ã�
    int opt = 1;
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    if (ret == -1) {
        perror("setsockopt");
        return -1;
    }
    // ��
    struct sockaddr_in sAddr;
    sAddr.sin_family = AF_INET;
    sAddr.sin_port = htons(port);
    sAddr.sin_addr.s_addr = INADDR_ANY;
    ret = bind(lfd, (struct sockaddr*)&sAddr, sizeof sAddr);
    if (ret == -1) {
        perror("bind");
        return -1;
    }
    // ���ü���
    ret = listen(lfd, 128);
    if (ret == -1) {
        perror("listen");
        return -1;
    }

    return lfd;
}

int EpollRun(int lfd)
{
    // ����epollʵ��
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create");
        return -1;
    }
    // lfd����
    struct epoll_event ev;
    ev.data.fd = lfd;
    ev.events = EPOLLIN;
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return -1;
    }
    // ���
    struct epoll_event evs[1024];
    int size = sizeof(evs) / sizeof(evs[0]);
    while (1) {
        int num = epoll_wait(epfd, evs, size, -1); // -1�����ȴ��¼�
        for (int i = 0; i < num; ++i) {
            int fd = evs[i].data.fd;
            ThreadParam* info = (ThreadParam*)malloc(sizeof(ThreadParam));
            info->epfd = epfd;
            info->fd = fd;
            if (fd == lfd) {
                // ��������
                pthread_create(&info->tid, NULL, AcceptClient, info);
            } else {
                // ��Ҫ���ܶԶ�����http
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
    // ���÷�����(��ֹ���ӵĿͻ���û�����ݷ���ʱ��read����)
    int flag = fcntl(cfd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);
    // ��ӵ�epoll
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET; // ����ˮƽģʽ->��Եģʽ
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
    recv������Ϊ���������������ݶ���󻹻����������
    recv���������ݷ���-1����ȡʧ�ܷ���-1
    ���ֵķ���ʱ��ͬ��perror
    */
    while ((len = recv(info->fd, tmp, sizeof tmp, 0)) > 0) {
        // ��ȷ�������п��Զ�ȡ��������get���ݿ��Ժ���
        if (total + len < sizeof buf) {
            memcpy(buf + total, tmp, (size_t)len);
        }
        total += len;
    }
    // �ж������Ƿ�������
    if (len == -1 && errno == EAGAIN) {
        // ����������
        char* pt = strstr(buf, "\r\n");
        long reqLen = pt - buf;
        buf[reqLen] = '\0';
        ParseHttpRequest(buf, info->fd);
    } else if (len == 0) {
        // �ͻ��˶Ͽ�����
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
    } else {
        perror("recv");
    }
    close(info->fd);

    return NULL;
}

int ParseHttpRequest(const char* line, int cfd)
{
    // ����������
    char method[12] = { 0 };
    char path[1024] = { 0 };
    sscanf(line, "%[^ ] %[^ ]", method, path);
    printf("method: %s, path: %s\r\n", method, path);
    if (strcasecmp(method, "get") != 0) {
        return -1;
    }
    // ���ͻ��˵����������utf-8ת�������ַ�
    DecodeHttpMsg(path, path);
    printf("method: %s, path: %s\r\n", method, path);

    // ����ͻ�������ľ�̬��Դ(Ŀ¼����Դ)
    char* file = NULL;
    if (strcmp(path, "/") == 0) {
        file = "./";
    } else {
        file = path + 1; // ��/ȥ��
    }
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1) {
        // �ļ������� -- �ظ�404
        SendHttpResponseHead(cfd, 404, "Not Found", GetFileType(".html"), -1);
        SendFile("404.html", cfd);
        return 0;
    }
    // �ж��ļ�����
    if (S_ISDIR(st.st_mode)) {
        // ��Ŀ¼���ݷ��͸��ͻ���
        SendHttpResponseHead(cfd, 200, "OK", GetFileType(".html"), -1);
        SendDir(file, cfd);
    } else {
        // ���ļ����ݷ��͸��ͻ���
        SendHttpResponseHead(cfd, 200, "OK", GetFileType(file), st.st_size);
        SendFile(file, cfd);
    }

    return 0;
}

int SendHttpResponseHead(int cfd, int status, const char* desc, const char* type, long length)
{
    // ״̬��
    char buf[4096] = { 0 };
    sprintf(buf, "http/1.1 %d %s\r\n", status, desc);

    // ��Ӧͷ
    sprintf(buf + strlen(buf), "content-type: %s\r\n", type);
    sprintf(buf + strlen(buf), "content-length: %d\r\n\r\n", length);

    send(cfd, buf, strlen(buf), 0);

    return 0;
}

int SendFile(const char* fileName, int cfd)
{
    // ��һ���֣���һ���֣�tcp��ʽЭ��
    int fd = open(fileName, O_RDONLY);
    assert(fd > 0);
#if 0
    while (1) {
        char buf[1024];
        int len = read(fd, buf, sizeof buf);
        if (len > 0) {
            send(cfd, buf, len, 0);
            usleep(10); // �ǳ���Ҫ����Ҫ���ͻ��˽���/�������ݵ�ʱ��
        } else if(len == 0) {
            break;
        } else {
            perror("read");
        }
    }
#else
    long size = lseek(fd, 0, SEEK_END); // ����ʹ��stat
    lseek(fd, 0, SEEK_SET);
    off_t offset = 0;
    while (offset < size) {
        printf("offset: %ld, totalSize:%ld\r\n", offset, size);
        ssize_t ret = sendfile(cfd, fd, &offset, (size_t)(size - offset)); // ***����cfd������Ϊ������ģʽ������send����ʱ����cfd��write��������ȡ���ݣ������fd�е����ݻ�û�б��ں�д�뵽cfd��write�Ļ���������ʱ�����ݾͻᱨ��ʱ�ļ�������
        printf("send size: %ld\r\n", ret);
        if (ret == -1 && errno == EAGAIN) {
            perror("sendfile");
            printf("cfd ��Ӧ�� write��������û�����ݡ�����\n");
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
    // nameListָ�� struct dirent* tmp[];
    struct dirent** nameList;
    int num = scandir(dirName, &nameList, NULL, alphasort);
    for (int i = 0; i < num; ++i) {
        // ȡ���ļ���
        char* name = nameList[i]->d_name;
        struct stat st;
        char subPath[1024] = { 0 };
        sprintf(subPath, "%s/%s", dirName, name);
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode)) {
            // a��ǩ<a href=""></a>>
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
    const char* dot = strrchr(fileName, '.'); // ����������.

    if (dot == NULL) {
        return "text/plain; charset=utf-8"; // ���ı�
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
            // ��16���Ƶ����� --> ʮ������ֵ
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
