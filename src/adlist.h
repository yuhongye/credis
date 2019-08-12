#ifndef __ADLIST_H__
#define __ADLIST_H__

// 遍历方向
#define AL_START_HEAD 0
#define AL_START_TAIL 1

/**
 * 双向链表node定义
 */
typedef struct ListNode {
    struct ListNode *prev;
    struct ListNode *next;
    void *value;
} ListNode;

typedef struct List {
    ListNode *head;
    ListNode *tail;

    void *(*dup)(void *ptr);
    void (*free)(void *ptr);
    int (*match)(void *ptr, void *key);
    unsigned int len;

} List;

typedef struct ListIter {
    ListNode *next;
    ListNode *prev;
    int direction;
} ListIter;

/** 使用宏来实现函数 */
#define listLength(l) ((l)->len)
#define listFirst(l) ((l)->head)
#define listLast(l) ((l)->tail)
#define listPrevNode(n) ((n)->prev)
#define listNextNode(n) ((n)->next)
#define listNodeValue(n) ((n)->value)

#define listSetDupMethod(l, m) ((l)->dup = (m))
#define listSetFreeMethod(l, m) ((l)->free = (m))
#define listSetMatchMethod(l, m) ((l)->match = (m))

#define listGetDupMethod(l) ((l)->dup)
#define listGetFree(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/** prototypes */
List *listCreate(void);
void listRelease(List *list);

List *listAddNodeHead(List *list, void *value);
List *listAddNodeTail(List *list, void *value);
void listDelNode(List *list, ListNode *node);

ListIter *listGetIterator(List *list, int direction);
ListNode *listNextElement(ListIter *iter); 
void listReleaseIterator(ListIter *iter);

List *listDup(List *origin);

ListNode *listSearchKey(List *list, void *key);
ListNode *listIndex(List *list, int index);


#endif