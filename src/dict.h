#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/** 消除not used compile warning */
#define DICT_NOUSED(V) ((void) V)

typedef struct DictEntry {
    void *key;
    void *val;
    struct DictEntry *next;
} DictEntry;

/**
 * DictType表示什么，不同类型的key/value?
 */
typedef struct DictType {
    unsigned int (*hashFunction)(const void *key);
    // TODO: privdata是什么?
    void *(*keyDup)(void *privdata, const void *key);
    void *(*valDup)(void *privdata, const void *obj);
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);
    void (*keyDestructor)(void *privdata, void *key);
    void (*valDestructor)(void *privdata, void *obj);
} DictType;

typedef struct Dict {
    DictEntry **table;
    DictType *type;
    // size表示的是capacity? 还是有多少kv?
    unsigned int size;
    unsigned int sizemask;
    unsigned int used;
    // what is it?
    void *privdata;
} Dict;

typedef struct DictIterator {
    // hash table
    Dict *ht;
    // 已经遍历过的bucket
    int index;
    DictEntry *entry, *nextEntry;
} DictIterator;

// dict的默认大小
#define DICT_INITIAL_SIZE 16

/********************************* Macros *****************************/
/**
 * 1. 对于要设置的(key, value)根据keyDup和valueDup是否为空来决定是否需要进行copy
 * 2. 在删除entry时，根据keyDestructor和valueDestructor来决定是否进行free
 */

#define dictFreeEntryVal(ht, entry) \
    if((ht)->type->valDestructor) \
        (ht)->type->valDestructor((ht)->privdata, (entry)->val)

/**
 * 为什么要叫setHashVal，就是set entry value，不如叫SetDictVal，因为对外暴露的是dict，而不是hash table
 * 实现dict的方式很多种啊, tree也可以
 */
#define dictSetHashVal(ht, entry, _val_) do { \
    if ((ht)->type->valDup) \
        entry->val = (ht)->type->valDup((ht)->privdata, _val_);\
    else \
        entry->val = (_val_);\
} while (0)

#define dictFreeEntryKey(ht, entry) \
    if ((ht)->type->keyDestructor) \
        (ht)->type->keyDestructor((ht)->privdata, (entry)->key)

#define dictSetHashKey(ht, entry, _key_) do { \
    if ((ht)->type->keyDup) \
        entry->key = (ht)->type->keyDup((ht)->privdata, _key_); \
    else \
        entry->key = (_key_);\
} while (0)

/** 为什么这里用起了三目运算符 */
#define dictCompareHashKeys(ht, key1, key2) \
    (((ht)->type->keyCompare) ? \
        (ht)->type->keyCompare((ht)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(ht, key) (ht)->type->hashFunction(key)

#define dictGetEntryKey(entry) ((entry)->key)
#define dictGetEntryVal(entry) ((entry)->val)
#define dictGetHashTableSize(ht) ((ht)->size)
#define dictGetHashTableUsed(ht) ((ht)->used)

/** api */
Dict *dictCreate(DictType *type, void *privDataPtr);
void dictEmpty(Dict *ht);
int dictExpand(Dict *ht, unsigned int size);
int dictAdd(Dict *ht, void *key, void *val);
int dictReplace(Dict *ht, void *key, void *val);
int dictDelete(Dict *ht, const void *key);
int dictDeleteNoFree(Dict *ht, const void *key);
void dictRelease(Dict *ht);
DictEntry *dictFind(Dict *ht, const void *key);
int dictResize(Dict *ht);

DictIterator *dictGetIterator(Dict *ht);
DictEntry *dictNext(DictIterator *it);
void dictReleaseIterator(DictIterator *it);

DictEntry *dictGetRandomKey(Dict *ht);

void dictPrintStats(Dict *ht);

unsigned int dictGenHashFunction(const unsigned char *buf, int len);

/** Hash table types */
extern DictType dictTypeHeapStringCopyKey;
extern DictType dictTypeHeapStrings;
extern DictType dictTypeHeapStringCopyKeyValue;

#endif