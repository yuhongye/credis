#ifndef __AE_H__
#define __AE_H__

struct aeEventLoop;

/** 文件事件和时间事件处理，和事件销毁器，定义函数类型 */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientdata, int mask);
/**
 * TODO: 返回值是什么，id是什么？
 */
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizeProc(struct aeEventLoop *eventLoop, void *clientData);

/** File Event struct */
typedef struct AeFileEvent {
    int fd;
    // AE_(READABLE|WRITABLE|EXCEPTION)
    int mask;
    aeFileProc *fileProc;
    aeEventFinalizeProc *finalizeProc;
    void *clientData;
    struct AeFileEvent *next;
} AeFileEvent;

typedef struct AeTimeEvent {
    // time event identifier
    long long id;
    long when_sec;
    long when_ms;
    aeTimeProc *timeProc;
    aeEventFinalizeProc *finalizeProc;
    void *clientData;
    struct AeTimeEvent *next;
} AeTimeEvent;

/** State of an event based program */
typedef struct AeEventLoop {
    long long timeEventNextId;
    AeFileEvent *fileEventHead;
    AeTimeEvent *timeEventHead;
    int stop;
} AeEventLoop;

#define AE_OK 0
#define AE_ERR 1

#define AE_READBLE 1
#define AE_WRITABLE 2
#define AE_EXCEPTION 4

#define AE_FILE_EVENT 1
#define AE_TIME_EVENT 2
#define AE_ALL_EVENT (AE_FILE_EVENT | AE_TIME_EVENT)
#define AE_DONT_WAIT 4

#define AE_NOMORE -1

#define AE_NOUSED(V) ((void) V)

/** Prototypes */
AeEventLoop *aeCreateEventLoop(void);
void aeDeleteEventLoop(AeEventLoop *eventLoop);
void aeStop(AeEventLoop *eventLoop);

int aeCreateFileEvent(AeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData, aeEventFinalizeProc *finalizeProc);
int aeDeleteFileEvent(AeEventLoop *eventLoop, int fd, int mask);

long long aeCreateTimeEvent(AeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, void *clientData, aeEventFinalizeProc *finalizeProc);
int aeDeleteTimeEvent(AeEventLoop *eventLoop, long long id);

int aeProcessEvents(AeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask , long long milliseconds);
void aeMain(AeEventLoop *eventLoop);

#endif