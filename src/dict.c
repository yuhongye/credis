#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "zmalloc.h"
#include "dict.h"

/*************************** Utility functions **********************/
static void _dictPanic(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "\nDict LIBRARY PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n\n");
    va_end(ap);
}

/************************ heap manager wrapper ********************/
static void *_dictAlloc(int size) {
    void *p = zmalloc(size);
    if (p == NULL) {
        _dictPanic("Out of memory");
    }
    return p;
}

static void _dictFree(void *ptr) {
    zfree(ptr);
}

/******************* private prototypes **************************/
static int _dictExpandIfNeeded(Dict *ht);
static unsigned int _dictNextPower(unsigned int size);
static int _dictKeyIndex(Dict *ht, const void *key);
static int _dictInit(Dict *ht, DictType *type, void *privDataPtr);

static int _dictEntryLen(DictEntry *entry);

/**
 * int型的hash计算函数： Thomas Wang's 32 bit Mix Function
 */
unsigned int dictIntHashFunction(unsigned int key) {
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;
}

/**
 * 既然是唯一的，那就不需要hash了
 */
unsigned int dictIdentityHashFunction(unsigned int key) {
    return key;
}

/**
 *  通用的hash计算函数，跟effective java中推荐的算法差不多
 */
unsigned int dictGenHashFunction(const unsigned char *buf, int len) {
    unsigned int hash = 5381;
    while (len--) {
        hash = ((hash << 5) + hash) + (*buf++);
    }
    return hash;
}

/********************** API implementation **********************/

/**
 * Reset an hashtable already initialized with ht_init().
 * @Notice: This function should only called by ht_destroy().
 */
static void _dictReset(Dict *ht) {
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/**
 * create a new hash table
 * 这里没有申请bucket的空间，等到add的时候会判断是否有足够的空间，不足时自动扩容
 */
Dict *dictCreate(DictType *type, void *privDataPtr) {
    Dict *ht = _dictAlloc(sizeof(*ht));
    _dictInit(ht, type, privDataPtr);
    return ht;
}

int _dictInit(Dict *ht, DictType *type, void *privDataPtr) {
    _dictReset(ht);
    ht->type = type;
    ht->privdata = privDataPtr;
    // TODO: 不设置size, mask, used?
    return DICT_OK;
}

/**
 * resize the table to the minimal size that contains all the elements
 * but with the invariant of a USER/BUCKETS ration near to <= 1
 */
int dictResize(Dict *ht) {
    int minimal = ht->used;
    if (minimal < DICT_INITIAL_SIZE) {
        minimal = DICT_INITIAL_SIZE;
    }
    return dictExpand(ht, minimal);
}

/**
 * rehash整个hashtable
 */
int dictExpand(Dict *ht, unsigned int size) {
    Dict newHt;
    unsigned int realSize = _dictNextPower(size);

    // 不能比ht已有的元素个数还要小
    if (ht->used > size) {
        return DICT_ERR;
    }

    _dictInit(&newHt, ht->type, ht->privdata);
    newHt.size = realSize;
    newHt.sizemask = realSize - 1;
    newHt.table = _dictAlloc(realSize * sizeof(DictEntry *));
    // 设置为null值
    memset(newHt.table, 0, realSize * sizeof(DictEntry *));

    /** rehash */
    newHt.used = ht->used;
    for (int i = 0; i < ht->size && ht->used > 0; i++) {
        if (ht->table[i] == NULL) {
            continue;
        }

        DictEntry *nextEntry;
        DictEntry *e = ht->table[i];
        while (e != NULL) {
            nextEntry = e->next;
            unsigned int h = dictHashKey(ht, e->key) & newHt.sizemask;
            // 头部插入
            e->next = newHt.table[i];
            newHt.table[i] = e;
            ht->used--;
            e = nextEntry;
        }
    }

    assert(ht->used == 0);
    _dictFree(ht->table);
    *ht = newHt;
    return DICT_OK;
}

/**
 * Add an element to the target hash table
 * TODO: 不需要检查是否有空间吗
 */
int dictAdd(Dict *ht, void *key, void *val) {
    int index = _dictKeyIndex(ht, key);
    // -1意味着key已经存在
    if (index == -1) {
        return DICT_ERR;
    }

    DictEntry *entry = _dictAlloc(sizeof(*entry));
    entry->next = ht->table[index];
    ht->table[index] = entry;

    // set key value
    dictSetHashKey(ht, entry, key);
    dictSetHashVal(ht, entry, val);
    
    ht->used++;
    return DICT_OK;
}

int dictReplace(Dict *ht, void *key, void *val) {
    // 如果不存在，则直接插入
    if (dictAdd(ht, key, val) == DICT_OK) {
        return DICT_OK;
    }

    // 已经存在
    DictEntry *entry = dictFind(ht, key);
    dictFreeEntryVal(ht, entry);
    dictSetHashVal(ht, entry, val);
    return DICT_OK;
}

/**
 * search and remove an element
 */
static int dictGenericDeleted(Dict *ht, const void *key, int nofree) {
    if (ht->size == 0) {
        return DICT_ERR;
    }

    unsigned int h = dictHashKey(ht, key) & ht->sizemask;
    DictEntry *entry = ht->table[h];

    DictEntry *prevEntry = NULL;
    while (entry != NULL) {
        // 找到了目标(key, value)
        if (dictCompareHashKeys(ht, key, entry->key)) {
            // 在链表中删除entry
            if (prevEntry != NULL) {
                prevEntry ->next = entry->next;
            } else {
                ht->table[h] = entry->next;
            }

            if (!nofree) {
                dictFreeEntryKey(ht, entry);
                dictFreeEntryVal(ht, entry);
            }
            _dictFree(entry);
            ht->used--;
            return DICT_OK;
        } else {
            prevEntry = entry;
            entry = entry->next;
        }
    }

    return DICT_ERR;
}

int dictDelete(Dict *ht, const void *key) {
    return dictGenericDeleted(ht, key, 0);
}

int dictDeleteNoFree(Dict *ht, const void *key) {
    return dictGenericDeleted(ht, key, 1);
}

/**
 * 释放掉所有的hash table entry
 */
int _dictClear(Dict *ht) {
    for (int i = 0; i < ht->size && ht->used > 0; i++) {
        DictEntry *entry = ht->table[i];
        if (entry == NULL) {
            continue;
        }
        
        DictEntry *nextEntry = NULL;
        while (entry != NULL) {
            nextEntry = entry->next;
            dictFreeEntryKey(ht, entry);
            dictFreeEntryVal(ht, entry);
            _dictFree(entry);
            ht->used--;
            entry = nextEntry;
        }

    }
    _dictFree(ht->table);
    _dictReset(ht);
    return DICT_OK;
}

void dictRelease(Dict *ht) {
    _dictClear(ht);
    _dictFree(ht);
}

/**
 * @return NULL if ht is empty or not found key, else the target entry
 */
DictEntry *dictFind(Dict *ht, const void *key) {
    if (ht->size == 0) {
        return NULL;
    }

    unsigned int h = dictHashKey(ht, key) & ht->sizemask;
    DictEntry *entry = ht->table[h];
    while (entry != NULL) {
        if (dictCompareHashKeys(ht, key, entry->key)) {
            return entry;
        } else {
            entry = entry->next;
        }
    }
    return NULL;
}

DictIterator *dictGetIterator(Dict *ht) {
    DictIterator *it = _dictAlloc(sizeof(*it));

    it->ht = ht;
    it->index = -1;
    it->entry = NULL;
    it->nextEntry = NULL;
    return it;
}

DictEntry *dictNext(DictIterator *it) {
    while (1) {
        if (it->entry == NULL) {
            it->index++;
            if (it->index >= (signed) it->ht->size) {
                break;
            }
            it->entry = it->ht->table[it->index];
        } else {
            it->entry = it->nextEntry;
        }

        if (it->entry != NULL)  {
            it->nextEntry = it->entry->next;
            return it->entry;
        }
    }
    return NULL;
}

void dictReleaseIterator(DictIterator *it) {
    // entry, nextEntry都是指向已经存在的entry，没有申请新的空间，因此不需要free
    _dictFree(it);
}

/**
 * 随机获取一个entry， 可以用来实现随机化算法
 */
DictEntry *dictGetRandomKey(Dict *ht) {
    // 是否应该改成ht->used == 0 ?
    if (ht->used == 0) {
        return NULL;
    }

    unsigned int h = random() & ht->sizemask;
    DictEntry *entry = ht->table[h];
    while (entry == NULL) {
        h = random() & ht->sizemask;
        entry = ht->table[h];
    }

    /** 现在我们获取到了一个非空的bucket，但是它是一个list，我们还要从list中随机获取一个 */
    int listlen = _dictEntryLen(entry);
    int index = random() % listlen;
    while (index--) {
        entry = entry->next;
    }
    return entry;
}

/****************************** private functions *****************************/

static int _dictExpandIfNeeded(Dict *ht) {
    if (ht->size == 0) {
        return dictExpand(ht, DICT_INITIAL_SIZE);
    }
    if (ht->used == ht->size) {
        return dictExpand(ht, ht->size * 2);
    }
    return DICT_OK;
}

static unsigned int _dictNextPower(unsigned int size) {
    if (size >= 2147483648U) {
        return 2147483648U;
    }
    unsigned int i = DICT_INITIAL_SIZE;
    while (1) {
        if (i >= size) {
            return i;
        }
        i *= 2;
    }
}

/**
 * @return the slot index of the key should be store in, or else -1 if key already exists
 */
static int _dictKeyIndex(Dict *ht, const void *key) {
    if (_dictExpandIfNeeded(ht) == DICT_ERR) {
        return -1;
    }

    unsigned int h = dictHashKey(ht, key) & ht->sizemask;
    DictEntry *entry = ht->table[h];
    while (entry != NULL) {
        if (dictCompareHashKeys(ht, entry->key, key)) {
            return -1;
        }
        entry = entry->next;
    }
    return h;
}

void dictEmpty(Dict *ht) {
    _dictClear(ht);
}

#define DICT_STATS_VECTLEN 50
void dictPrintStats(Dict *ht) {
    if (ht->used == 0) {
        printf("No stats available for empty dictionaries\n");
        return;
    }

    unsigned int clvector[DICT_STATS_VECTLEN];
    for (int i = 0; i < DICT_STATS_VECTLEN; i++) {
        clvector[i] = 0;
    }

    unsigned int nonEmptySlots = 0;
    unsigned int maxChainLen = 0;
    unsigned int totalChainLen = 0;
    for (int i = 0; i < ht->size; i++) {
        if (ht->table[i] != NULL) {
            nonEmptySlots++;
        }
        int chainLen = _dictEntryLen(ht->table[i]);
        int index = chainLen < DICT_STATS_VECTLEN ? chainLen : (DICT_STATS_VECTLEN - 1);
        clvector[index]++;
        if (chainLen > maxChainLen) {
            maxChainLen = chainLen;
        }
        totalChainLen += chainLen;
    }

    printf("Hash table stats:\n");
    printf("  table size: %d\n", ht->size);
    printf("  number of elements: %d\n", ht->used);
    printf("  different slots: %d\n", nonEmptySlots);
    printf("  max chain length: %d\n", maxChainLen);
    printf("  avg chain length(counted): %.02f\n", (float) totalChainLen / nonEmptySlots);
    printf("  avg chain length (computed): %.02f\n", (float)ht->used / nonEmptySlots); // 这两者应该相同吧?
    printf("  Chain length distribution: \n");
    for (int i = 0; i < DICT_STATS_VECTLEN; i++) {
        if (clvector[i] != 0) {
            printf("    %s%d: %d(%0.2f%%)\n", (i ==  DICT_STATS_VECTLEN-1) ? ">=" : "", i, clvector[i], ((float)clvector[i]/ht->size) * 100);
        }
    }
}

static int _dictEntryLen(DictEntry *entry) {
    int len = 0;
    while (entry != NULL) {
        len++;
        entry = entry->next;
    }
    return len;
}

/** ------------------------ StringCopy Hash Table Type -------------------- */
static unsigned int _dictStringCopyHTHashFunction(const void *key) {
    return dictGenHashFunction(key, strlen(key));
}

static void *_dictStringCopyHTKeyDup(void *privdata, const void *key) {
    int len = strlen(key);
    char *copy = _dictAlloc(len+1);
    DICT_NOUSED(privdata);

    memcpy(copy, key, len);
    copy[len] = '\0';
    return copy;
}

static void *_dictStringKeyValCopyHTValDup(void *privdata, const void *val) {
    int len = strlen(val);
    char *copy = _dictAlloc(len+1);
    DICT_NOUSED(privdata);

    memcpy(copy, val, len);
    copy[len] = '\0';
    return copy;
}

static int _dictStringCopyHTKeyCompare(void *privdata, const void *key1, const void *key2) {
    DICT_NOUSED(privdata);
    // 返回的值变成了是否相等
    return strcmp(key1, key2) == 0;
}

static void _dictStringCopyHTKeyDestructor(void *privdata, void *key) {
    DICT_NOUSED(privdata);
    _dictFree(key);
}

static void _dictStringKeyValCopyHTValDestructor(void *privdata, void *value) {
    DICT_NOUSED(privdata);
    _dictFree(value);
}

/**
 * add时不拷贝val，remove时不free val
 */
DictType dictTypeHeapStringCopyKey = {
    _dictStringCopyHTHashFunction,
    _dictStringCopyHTKeyDup,
    NULL, // val dup
    _dictStringCopyHTKeyCompare,
    _dictStringCopyHTKeyDestructor,
    NULL // val destructor
};

/**
 * 不拷贝key，但是free key
 */
DictType dictTypeHeapStrings = {
    _dictStringCopyHTHashFunction, 
    NULL, // key dup
    NULL, // val dup
    _dictStringCopyHTKeyCompare,
    _dictStringCopyHTKeyDestructor,
    NULL // val destructor
};

/**
 * copy key value, free key value
 */
DictType dictTypeHeapStringCopyKeyValue = {
    _dictStringCopyHTHashFunction,
    _dictStringCopyHTKeyDup,
    _dictStringKeyValCopyHTValDup,
    _dictStringCopyHTKeyCompare,
    _dictStringCopyHTKeyDestructor,
    _dictStringKeyValCopyHTValDestructor
};


/*************************** debug *****************************/

void display(Dict *dict) {
    DictIterator *it = dictGetIterator(dict);
    DictEntry *entry = dictNext(it);
    printf("dict size: %d\n", dict->used);
    while (entry != NULL) {
        printf("\t%s: %s\n", entry->key, entry->val);
        entry = dictNext(it);
    }

    dictReleaseIterator(it);
}

int main() {
    void *privdata = _dictAlloc(1);
    Dict *dict = dictCreate(&dictTypeHeapStringCopyKeyValue, privdata);
    dictAdd(dict, "name", "cxy");
    char key[] = {'n', 'a', 'm', 'e', 'x',  '1', '1', '1','\0'};
    char value[] = {'c', 'x', 'y', 'x', '1', '1', '1', '\0'};
    int start = 'a';
    for (int i = 0; i < 20190801; i++) {
        key[4] = (start + i);
        value[3] = (start + i);
        dictAdd(dict, key, value);
    }
    // display(dict);
    dictPrintStats(dict);

    return 0;
}




