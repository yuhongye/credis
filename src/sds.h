/*************************************************************************
	> File Name: sds.h
	> Author: caoxiaoyong
	> Mail: caoxyemail@gmail.com
	> Created Time: 三  7/17 17:36:46 2019
 ************************************************************************/

#ifndef __SDS_H
#define __SDS_H

#include <sys/types.h>

/**
 * Notice: remember sds is a char pointer
 */
typedef char *sds;

struct sdshdr {
    long len;
    long free;
    // flexible array
    char buf[0];
};

/**
 * use init to initialize a new sds, the sds size is initlen
 */
sds sdsnewlen(const void *init, size_t initlen);
/**
 * use init to initialize a new sds
 */
sds sdsnew(const char *init);

/**
 * create an empty sds
 */
sds sdsempty();

/**
 * @return length of s
 */
size_t sdslen(const sds s);

/**
 * deep copy s
 */
sds sdsdup(sds s);

void sdsfree(sds s);

/**
 * left space of s
 */
size_t sdsavail(sds s);

/**
 * 将t的len字节追加到s后面
 */
sds sdscatlen(sds s, void *t, size_t len);

sds sdscat(sds s, char *t);

/**
 * 跟sdscatlen的区别是什么？
 */
sds sdscpylen(sds s, char *t, size_t len);

sds sdscpy(sds s, char *t);

sds sdscatprintf(sds s, const char *fmt, ...);

sds sdstrim(sds s, const char *cset);

sds sdsrange(sds s, long start, long end);

/**
 * 什么情况下需要update length
 */
void sdsupdatelen(sds s);

int sdscmp(sds s1, sds s2);

sds *sdssplitlen(char *s, int len, char *sep, int seplen, int *count);

void sdstolower(sds s);

#endif
