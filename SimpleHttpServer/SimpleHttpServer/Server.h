#ifndef SERVER_H
#define SERVER_H

// ��ʼ�����ڼ������׽���
int InitListenFd(unsigned short port);
// ����epoll
int EpollRun(int lfd);
// �Ϳͻ��˽�������
void* AcceptClient(void* arg);
// ����http request
void* RecvHttpRequest(void* arg);
// ����http request
int ParseHttpRequest(const char* line, int cfd);
// ����http response(״̬�� + ��Ӧͷ)
int SendHttpResponseHead(int cfd, int status, const char* desc, const char* type, long length);
// �����ļ�
int SendFile(const char* fileName, int cfd);
// ����Ŀ¼
int SendDir(const char* dirName, int cfd);

const char* GetFileType(const char* fileName);
void DecodeHttpMsg(char* to, char* from);
int HexToDec(char c);


#endif // !SERVER_H
