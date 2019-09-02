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
long long aeCreateTimeEvent(AeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, 
                            void *clientData, aeEventFinalizeProc *finalizeProc) {
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

static void addFileEventToFdsets(AeFileEvent *fe, fd_set *rfds, fd_set *wfds, fd_set *efds) {
    if (fe->mask & AE_READBLE) {
        FD_SET(fe->fd, rfds);
    }
    if (fe->mask & AE_WRITABLE) {
        FD_SET(fe->fd, wfds);
    }
    if (fe->mask & AE_EXCEPTION) {
        FD_SET(fe->fd, efds);
    }
}

/**
 * 获取select时应该等待的时间
 */
static struct timeval *getSelectTimeval(AeEventLoop *eventLoop, int flags, struct timeval *tvp) {
    AeTimeEvent *shortest = NULL;
    if (flags & AE_TIME_EVENT && !(flags & AE_DONT_WAIT)) {
        shortest = aeSearchNearestTime(eventLoop);
    }
    if (shortest != NULL) {
        long now_sec, now_ms;
        aeGetTime(&now_sec, &now_ms);
        // shortest不应该小于now吗，tvp的数据都是负数？
        tvp->tv_sec = shortest->when_sec - now_sec;
        if (shortest->when_ms < now_ms) {
            tvp->tv_usec = ((shortest->when_ms+1000) - now_ms) * 1000;
            tvp->tv_sec--;
        } else {
            tvp->tv_usec += (shortest->when_ms - now_ms) * 1000;
        }
        return tvp;
    } else {
        // 不等待，立即返回
        if (flags & AE_DONT_WAIT) {
            tvp->tv_sec = tvp->tv_usec = 0;
            return tvp;
        } else {
            // wait forever
            return NULL;
        }
    }
}

/**
 * 传的参数不是太多了
 * @return 是否有时间发生
 */
static int fileEventAccur(AeEventLoop *eventLoop, AeFileEvent *fe, fd_set *rfds, fd_set *wfds, fd_set *efds) {
    int fd = fe->fd;
    int hasEvent =(fe->mask & AE_READBLE && FD_ISSET(fd, rfds)) || 
                  (fe->mask & AE_WRITABLE && FD_ISSET(fd, wfds)) ||
                  (fe->mask & AE_EXCEPTION && FD_ISSET(fd, efds));

    if (hasEvent) {
         int mask = 0;
        if (fe->mask & AE_READBLE && FD_ISSET(fd, &rfds)) {
            mask |= AE_READBLE;
        }
        if (fe->mask & AE_WRITABLE && FD_ISSET(fd, &wfds)) {
            mask |= AE_WRITABLE;
        }
        if (fe->mask & AE_EXCEPTION && FD_ISSET(fd, &efds)) {
            mask |= AE_EXCEPTION;
        }
        fe->fileProc(eventLoop, fe->fd, fe->clientData, mask);
        FD_CLR(fd, rfds);
        FD_CLR(fd, wfds);
        FD_CLR(fd, efds);
    }

    return hasEvent;
}

static void procTimeEvent(AeEventLoop *eventLoop, int flags) {
    if (!(flags & AE_TIME_EVENT)) {
        return;
    }

    AeTimeEvent *te = eventLoop->timeEventHead;
    long long maxId = eventLoop->timeEventNextId-1;
    while (te != NULL) {
        if (te->id > maxId) {
            te = te->next;
            continue;
        }

        long now_sec, now_ms;
        long long id;
        aeGetTime(&now_sec, &now_ms);
        // 难道当前事件不是一定会大于te的时间吗
        if (now_sec > te->when_sec || (now_sec == te->when_sec && now_ms >= te->when_ms)) {
            id = te->id;
            int retval = te->timeProc(eventLoop, id, te->clientData);
            /** 当我们处理了一个事件，time event list可能会发生变化 */
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval, &te->when_sec, &te->when_ms);
            } else {
                aeDeleteTimeEvent(eventLoop, id);
            }
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    } 
}

/**
 * 1. 统计fd的个数， 把fd加入到对应的fd_set中
 * 2. select获取事件
 * 3. 处理文件事件
 * 4. 处理时间事件
 */
int aeProcessEvents(AeEventLoop *eventLoop, int flags) {
    if (!(flags & AE_TIME_EVENT) && !(flags & AE_FILE_EVENT)) {
        return 0;
    }
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    int processed = 0;
    /** Check file events */
    AeFileEvent *fe = eventLoop->fileEventHead;
    int numfd = 0;
    int maxfd = 0;
    // 1. 统计fd；2. 发现max fd；3. 把fd放到对应的fd_set中
    if (flags & AE_FILE_EVENT) {
        while (fe != NULL) {
            addFileEventToFdsets(fe, &rfds, &wfds, &efds);
            if (maxfd < fe->fd) {
                maxfd = fe->fd;
            }
            numfd++;
            fe = fe->next;
        }
    }

    /** 即时没有file event，也需要处理time event */
    if (numfd > 0 || ((flags & AE_TIME_EVENT) && !(flags & AE_DONT_WAIT))) {
        struct timeval tv, *tvp;
        tvp = getSelectTimeval(eventLoop, flags, &tv);
        int retval = select(maxfd+1, &rfds, &wfds, &efds, tvp);
        if (retval > 0) {
            fe = eventLoop->fileEventHead;
            while (fe != NULL) {
                int hasEvent = fileEventAccur(eventLoop, fe, &rfds, &wfds, &efds);
                if (hasEvent) {
                    processed++;
                    /**
                    * 当处理完一个事件后，file event list可能会发生变化
                    */
                    fe = eventLoop->fileEventHead;
                } else {
                    fe = fe->next;
                }
            }
        }
    }

    /** check time events */
    procTimeEvent(eventLoop, flags);
   
    return processed;
}

/**
 * wait for milliseconds until the given file descriptor becomes writable/readable/exception
 */
int aeWait(int fd, int mask, long long milliseconds) {
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    struct timeval tv;
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds%1000) * 1000;
    
    if (mask & AE_READBLE) {
        FD_SET(fd, &rfds);
    }
    if (mask & AE_WRITABLE) {
        FD_SET(fd, &wfds);
    }
    if (mask & AE_EXCEPTION) {
        FD_SET(fd, &efds);
    }

    int retval = select(fd+1, &rfds, &wfds, &efds, &tv);
    int remask = 0;
    if (retval > 0) {
        if (FD_ISSET(fd, &rfds)) {
            remask |= AE_READBLE;
        }
        if (FD_ISSET(fd, &wfds)) {
            remask |= AE_WRITABLE;
        }
        if (FD_ISSET(fd, &efds)) {
            remask |= AE_EXCEPTION;
        }
        return remask;
    } else {
        return retval;
    }
}

void aeMain(AeEventLoop *eventLoop) {
    // 这样不一下子就跳出了吗
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        aeProcessEvents(eventLoop, AE_ALL_EVENT);
    }
}