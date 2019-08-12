/*************************************************************************
	> File Name: sds.c
	> Author: caoxiaoyong
	> Mail: caoxyemail@gmail.com
	> Created Time: 二  7/30 16:46:10 2019
 ************************************************************************/

#include "sds.h"
#include "zmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

static void sdsOOMAbort(void) {
    fprintf(stderr, "SDS: Out Of Memory (SDS_ABORT_ON_OOM defined)\n");
    // 定义在stdlib中，终止程序的执行，从调用处跳出
    abort();
}

/**
 * 申请指定大小的空间
 * 如果没有足够的空间且又指定了abort，则abort
 */
void* malloc_or_abort(size_t size) {
    void* buf = zmalloc(size);
#ifdef SDS_ABORT_ON_OOM
    if (buf == NULL) {
        sdsOOMAbort();
    }
#endif
    return buf;
}

/**
 * 获取sds的header
 */
struct sdshdr *header(const sds s) {
    return (void*) (s - (sizeof(struct sdshdr)));
}

/**
 * 创建一个长度为initlen的sds, 并将init中的initlen个字节拷贝到sds中
 * 不会对init和initlen做check, 因为是一个server而不是一个lib，所有的调用都在可控范围内?
 */
sds sdsnewlen(const void *init, size_t initlen) {
    struct sdshdr *sh;
    sh = zmalloc(sizeof(struct sdshdr) + initlen + 1);
#ifdef SDS_ABORT_ON_OOM
    if (sh == NULL) {
        sdsOOMAbort();
    }
#else
    if (sh == NULL) {
        return NULL;
    }
#endif

    sh->len = initlen;
    sh->free = 0;
    if (initlen) {
        if (init) {
            memcpy(sh->buf, init, initlen);
        } else {
            memset(sh->buf, 0, initlen);
        }
    }

    sh->buf[initlen] = '\0';
    // 注意: 返回的是buf, buf减去sdshdr的长度就得到sdshdr。heap中的内存是往上增长的
    return (char*) sh->buf;
}

sds sdsempty(void) {
    return sdsnewlen("", 0);
}

sds sdsnew(const char *init) {
    size_t initlen = init == NULL ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

size_t sdslen(const sds s) {
    struct sdshdr *sh = header(s);
    return sh->len;
}

sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/**
 * 释放sds，包括sdshdr
 */
void sdsfree(sds s) {
    if (s == NULL) {
        return;
    }
    zfree(s - sizeof(struct sdshdr));
}

size_t sdsavail(sds s) {
    struct sdshdr *sh = header(s);
    return sh->free;
}

void sdsupdatelen(sds s) {
    struct sdshdr *sh = header(s);
    int reallen = strlen(s);
    sh->free += (sh->len - reallen);
    sh->len = reallen;
}

/**
 * 如果s free空间大于addlen，则什么也不做
 * 否则话扩大为(len+addlen)的两倍, len+addlen就是接下来要存储的数据的长度
 * @Notice free字段会被正确更新
 * @param addlen 新增的空间大小
 */
static sds sdsMakeRoomFor(sds s, size_t addlen) {
    size_t free = sdsavail(s);
    if (free >= addlen) {
        return s;
    }

    size_t len = sdslen(s);
    struct sdshdr *sh = header(s);
    size_t newLen = (len + addlen) * 2;

    struct sdshdr *newsh = zrealloc(sh, sizeof(struct sdshdr) + newLen + 1);
#ifdef SDS_ABORT_ON_OOM
    if (newsh == NULL) {
        sdsOOMAbort();
    }
#else 
    if (newsh == NULL) {
        return NULL;
    }
#endif
    newsh->free = newLen - len;
    return newsh->buf;
}

/**
 * 从t中拷贝len个字节到s中
 */
sds sdscatlen(sds s, void *t, size_t len) {
    s = sdsMakeRoomFor(s, len);
    if (s == NULL) {
        return NULL;
    }
    size_t curlen = sdslen(s);
    struct sdshdr *sh = header(s);
    memcpy(s+curlen, t, len);
    sh->len = curlen + len;
    sh->free = sh->free - len;
    s[curlen+len] = '\0';
    return s;
}

sds sdscat(sds s, char *t) {
    return sdscatlen(s, t, strlen(t));
}

sds sdscpy(sds s, char *t) {
    return sdscpylen(s, t, strlen(t));
}

/**
 * 从t中拷贝len个字节覆盖s原来的值
 */
sds sdscpylen(sds s, char *t, size_t len) {
    struct sdshdr *sh = header(s);
    size_t totallen = sh->free + sh->len;
    
    if (totallen < len) {
        s = sdsMakeRoomFor(s, len-totallen);
        if (s == NULL) {
            return NULL;
        }
        sh = header(s);
        totallen = sh->free + sh->len;
    }

    memcpy(s, t, len);
    s[len] = '\0';
    sh->len = len;
    sh->free = totallen - len;
    return s;
}

sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *buf;
    char *t;
    size_t buflen = 32;

    while (1) {
        buf = malloc_or_abort(buflen);
        if (buf == NULL) {
            return NULL;
        }
        // TODO: 为什么是buflen-2
        buf[buflen-2] = '\0';

        va_start(ap, fmt);
        vsnprintf(buf, buflen, fmt, ap);
        va_end(va);
        if (buf[buflen-2] != '\0') {
            zfree(buf);
            buflen *= 2;
            continue;
        } else {
            break;
        }
    }

    t = sdscat(s, buf);
    zfree(buf);
    return t;
}


void display(sds s) {
    struct sdshdr *sh = header(s);
    printf("%s\n", sh->buf);
}

int main() {
    char *c = "caoxiaoyong";
    sds s = sdsnewlen(c, 11);
    display(s);
    display(sdsdup(s));

    return 0;
}


