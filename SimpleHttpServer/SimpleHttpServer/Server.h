#ifndef SERVER_H
#define SERVER_H

// 初始化用于监听的套接字
int InitListenFd(unsigned short port);
// 启动epoll
int EpollRun(int lfd);
// 和客户端建立连接
void* AcceptClient(void* arg);
// 接受http request
void* RecvHttpRequest(void* arg);
// 解析http request
int ParseHttpRequest(const char* line, int cfd);
// 发送http response(状态行 + 响应头)
int SendHttpResponseHead(int cfd, int status, const char* desc, const char* type, long length);
// 发送文件
int SendFile(const char* fileName, int cfd);
// 发送目录
int SendDir(const char* dirName, int cfd);

const char* GetFileType(const char* fileName);
void DecodeHttpMsg(char* to, char* from);
int HexToDec(char c);


#endif // !SERVER_H
