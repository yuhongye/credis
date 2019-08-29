#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

#include "ae.h"
#include "zmalloc.h"

AeEventLoop *aeCreateEventLoop(void) {
    AeEventLoop *eventLoop = zmalloc(sizeof(*eventLoop));
    if (eventLoop == NULL) {
        return NULL;
    }

    eventLoop->fileEventHead = NULL;
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    return eventLoop;
}

void aeDeleteEventLoop(AeEventLoop *eventLoop) {
    zfree(eventLoop);
}

void aeStop(AeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

int aeCreateFileEvent(AeEventLoop *eventLoop, int fd, int mask, 
                      aeFileProc *proc, void *clientData, aeEventFinalizeProc *finalizeProc) {
    AeFileEvent *fe = zmalloc(sizeof(*fe));
    if (fe == NULL) {
        return AE_ERR;
    }

    fe->fd = fd;
    fe->mask = mask;
    fe->fileProc = proc;
    fe->clientData = clientData;
    fe->finalizeProc = finalizeProc;
    fe->next = eventLoop->fileEventHead;
    eventLoop->fileEventHead = fe;
    return AE_OK;
}

void aeDeleteFileEvent(AeEventLoop *eventLoop, int fd, int mask) {
    AeFileEvent *prev = NULL;
    AeFileEvent *fe = eventLoop->fileEventHead;
    while (fe != NULL) {
        if (fe->fd == fd && fe->mask == mask) {
            if (prev == NULL) {
                eventLoop->fileEventHead = fe->next;
            } else {
                prev->next = fe->next;
            }
            if (fe->finalizeProc != NULL) {
                fe->finalizeProc(eventLoop, fe->clientData);
            }

            zfree(fe);
            return;
        }
        prev = fe;
        fe = fe->next;
    }
}

/**
 * 获取timestamp的秒数和不足1s的ms数
 * 比如1244770435 + 442ms
 */
static void aeGetTime(long *second, long *milliseconds) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *second = tv.tv_sec;
    *milliseconds = tv.tv_usec / 1000;
}

/**
 * 在当前的时间戳上添加指定的毫秒数
 */
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms;
    // 获取当前事件
    aeGetTime(&cur_sec, &cur_ms);

    // 当前事件+delete ms
    long when_sec = cur_sec + milliseconds/1000;
    long when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

/**
 * @param milliseconds: 这个参数是干嘛用的
 * @return time event id
 */
long long aeCreateTimeEvent(AeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, void *clientData, aeEventFinalizeProc *finalizeProc) {
    long long id = eventLoop->timeEventNextId++;
    AeTimeEvent *te = zmalloc(sizeof(*te));
    if (te == NULL) {
        return AE_ERR;
    }
    te->id = id;
    aeAddMillisecondsToNow(milliseconds, &te->when_sec, &te->when_ms);
    te->timeProc = proc;
    te->finalizeProc = finalizeProc;
    te->clientData = clientData;
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}

/**
 * @return delete success or not, AE_OK or AE_ERR
 */
int aeDeleteTimeEvent(AeEventLoop *eventLoop, long long id) {
    AeTimeEvent *te = eventLoop->timeEventHead;
    AeTimeEvent *prev = NULL;

    while (te != NULL) {
        if (te->id == id) {
            if (prev == NULL) {
                eventLoop->timeEventHead = te->next;
            } else {
                prev->next = te->next;
            }
            if (te->finalizeProc != NULL) {
                te->finalizeProc(eventLoop, te->clientData);
            }
            zfree(te);
            return AE_OK;
        }
        prev = te;
        te = te->next;
    }
    
    return AE_ERR;
}

/**
 * 找到最老的那个time event
 * @return NULL if there is no times, O(n)
 */
static AeTimeEvent *aeSearchNearestTime(AeEventLoop *eventLoop) {
    AeTimeEvent *te = eventLoop->timeEventHead;
    AeTimeEvent *nearest = NULL;
    while (te != NULL) {
        // te的时间早于nearst
        if (te == NULL || te->when_sec < nearest->when_sec || 
            (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms)) { 
               nearest = te;
        }
        te = te->next;
    }

    return nearest;
}

int aeProcessEvents(AeEventLoop *eventLoop, int flags) {
    if (!(flags & AE_TIME_EVENT) && !(flags & AE_FILE_EVENT)) {
        return 0;
    }
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    /** Check file events */
    AeFileEvent *fe = eventLoop->fileEventHead;
    int numfd = 0;
    if (flags & AE_FILE_EVENT) {
        while (fe != NULL) {
            if (fe->mask & AE_READBLE) {
                FD_SET(fe->fd, &rfds);
            }
            if (fe->mask & AE_WRITABLE) {
                FD_SET(fe->fd, &wfds);
            }
            if (fe->mask & AE_EXCEPTION) {
                FD_SET(fe->fd, &efds);
            }
            numfd++;
            fe = fe->next;
        }
    }


}