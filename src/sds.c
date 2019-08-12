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
    struct sdshdr *sh = malloc_or_abort(sizeof(struct sdshdr) + initlen + 1);
    if (sh == NULL) {
        return NULL;
    }

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
        va_end(ap);
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

/**
 * 去除sds前面和后面的cset部分
 * 使用了strchr函数, 因此只要连续的开头和结尾字符在cset里面，就会trim掉，而不需要跟cset的顺序一致
 * 所以cset这个名字起的还是很有意义的
 */
sds sdstrim(sds s, const char *cset) {
    struct sdshdr *sh = header(s);
    char *start, *end;
    char *sp, *ep;

    sp = start = s;
    ep = end =  s + sdslen(s) - 1;
    while (sp <= end && strchr(cset, *sp)) {
        sp++;
    }
    while (ep > start && strchr(cset, *ep)) {
        ep--;
    }
    size_t len = (sp > ep) ? 0 : (ep-sp + 1);

    // 前面trim
    if (sh->buf != sp) {
        memmove(sh->buf, sp, len);
    }

    sh->buf[len] = '\0';
    sh->free = sh->free + (sh->len - len);
    sh->len = len;
    return s;
}

/**
 * 获取实际的index，当index < 0时表示从后往前数
 * @param index 指定的index
 * @param 总共的长度
 */
long backwardOrZeor(long index, long totalSize) {
    if (index < 0) {
        index += totalSize;
    }
    if (index < 0) {
        // 这个地方失败是不是更合理
        index = 0;
    }
    return index;
}

/**
 * 仅保留[start,end]部分的内容
 * start和end如果小于0,意味着下标是从后往前数
 */
sds sdsrange(sds s, long start, long end) {
    struct sdshdr *sh = header(s);
    size_t len = sdslen(s);
    if (len == 0) {
        return s;
    }

    start = backwardOrZeor(start, len);
    end = backwardOrZeor(end, len);

    size_t newlen = 0;
    if (start <= end) {
        // 如果start和end超出了有效范围，则会保留最后一个字符，这是为了什么?
        if (start >= (signed)len) {
            start = len - 1;
        }
        if (end >= (signed)len) {
            end = len - 1;
        }
        // TODO: 这样合理吗？已经超出index范围了，为什么还要操作
        newlen = (start > end) ? 0 : (end - start) + 1;
    } else {
        start = 0;
    }

    if (start != 0) {
        memmove(sh->buf, sh->buf + start, newlen);
    }
    sh->buf[newlen] = 0;
    sh->free += sh->len - newlen;
    sh->len = newlen;

    return s;
}

void sdstolower(sds s) {
    int len = sdslen(s);
    for (int i = 0; i < len; i++) {
        s[i] = tolower(s[i]);
    }
}

/**
 * 比较两个sds
 */
void sdstoupper(sds s) {
    int len = sdslen(s);
    for (int i = 0; i < len; i++) {
        s[i] = toupper(s[i]);
    }
}

int sdscmp(sds s1, sds s2) {
    size_t len1 = sdslen(s1);
    size_t len2 = sdslen(s2);
    size_t min = (len1 < len2) ? len1 : len2;

    size_t cmp = memcmp(s1, s2, min);
    return cmp == 0 ? len1 - len2 : cmp;
}

/**
 * 使用sep来spit s
 *
 * @param count 分割后产生的结果大小，这个函数会设置它的值
 * @return 分割后的sds array
 */
sds *sdssplitlen(char *s, int len, char *sep, int seplen, int *count) {
    int slots = 5;
    sds *tokens = malloc_or_abort(sizeof(sds) * slots);
    if(seplen < 1 || len < 0 || tokens == NULL) {
        return NULL;
    }

    int elements = 0;
    int start = 0;
    for (int i = 0; i < (len - (seplen-1)); i++) {
        if (slots < elements + 2) {
            slots *= 2;
            sds *newtokens = zrealloc(tokens, sizeof(sds) * slots);
            if (newtokens == NULL)  {
                #ifdef SDS_ABORT_ON_OOM
                sdsOOMAbort();
                #else
                goto cleanup;
                #endif
            }
            tokens = newtokens;
        }

        if ((seplen == 1 && *(s+i) == sep[0]) || (memcmp(s+i, sep, seplen) == 0)) {
            tokens[elements] = sdsnewlen(s+start, i - start);
            if (tokens[elements] == NULL) {
                goto cleanup;
            }
            elements++;
            // 下一个token开始的位置
            start = i + seplen;
            // -1是因为接下来还要++
            i += seplen - 1;
        }
    }

    // 添加最后一个token，因为之前判断扩容的时候是<elements+2，因此tokens里面一定是未位置的
    tokens[elements] = sdsnewlen(s+start, len-start);
    if (tokens[elements] == NULL) {
        goto cleanup;
    }

    *count = ++elements;
    return tokens;

    #ifndef SDS_ABORT_ON_OOM
    cleanup: {
        for (int i = 0; i < elements; i++) {
            sdsfree(tokens[i]);
        }
        zfree(tokens);
        return NULL;
    }
    #endif
}

/** debug */
void display(sds s) {
    struct sdshdr *sh = header(s);
    printf("s value  : %s\n", sh->buf);
    printf("s length : %ld\n", sh->len);
    printf("s free   : %ld\n", sh->free);
}

int main() {
    sds s = sdsempty();
    display(s);

    sdscpy(s, "caoxy");
    printf("\nafter copy caoxy\n");
    display(s);

    sdscat(s, "email");
    printf("\n after concat email\n");
    display(s);

    sdscatlen(s, "@email.combalabala", 10);
    printf("\n after concat @email.combalala front 10 chars\n");
    display(s);

    sdsfree(s);

    s = sdsnew("airbnb cao xiao yong welcom airbnb");
    sdstrim(s, " airbnb");
    printf("\n after trim airbnb \n");
    display(s);

    int *count = malloc_or_abort(sizeof(int));
    sds *tokens = sdssplitlen(s, sdslen(s), " ", 1, count);
    printf("\nsplit count: %d\n", *count);
    for (int i = 0; i < *count; i++) {
        display(tokens[i]);
        putchar('\n');
    }

    sdsrange(s, 100, 100);
    printf("\n after range\n");
    display(s);

    return 0;
}


