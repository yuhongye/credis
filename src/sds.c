/*************************************************************************
	> File Name: sds.c
	> Author: caoxiaoyong
	> Mail: caoxyemail@gmail.com
	> Created Time: 二  7/30 16:46:10 2019
 ************************************************************************/

#include "sds.h"
#include "zmalloc.h"

#include<stdio.h>
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
    struct sdshdr *sh = (void*) (s - (sizeof(struct sdshdr)));
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
    struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
    return sh->free;
}

void sdsupdatelen(sds s) {
    struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
    int reallen = strlen(s);
    sh->free += (sh->len - reallen);
    sh->len = reallen;
}

/**
 * @param addlen 新增的空间大小
 */
static sds sdsMakeRoomFor(sds s, size_t addlen) {
    size_t free = sdsavail(s);
    if (free >= addlen) {
        return s;
    }

    size_t len = sdslen(s);
    struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
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

struct sdshdr *header(const sds s) {
    return (void*) (s - sizeof(struct sdshdr));
}

void display(sds s) {
    // struct sdshdr *sh = (void*)(s - (sizeof(struct sdshdr)));
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


