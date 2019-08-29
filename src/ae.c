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
 * 获取此时的秒数和毫秒数
 */
static void aeGetTime(long *second, long *milliseconds) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *second = tv.tv_sec;
    *milliseconds = tv.tv_usec / 1000;
}
