#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/**
 * 创建一个新的list
 * 
 * @return the pointer to the new list or NULL when error
 */
List *listCreate(void) {
    struct List *list = zmalloc(sizeof(*list));
    if (list != NULL) {
        // 没有哨兵节点
        list->head = list->tail = NULL;
        list->len = 0;
        list->dup = NULL;
        list->free = NULL;
        list->match = NULL;
    }

    return list;
}

/**
 * 释放整个list
 */
void listRelease(List *list) {
    unsigned int len;
    ListNode *current, *next;

    current = list->head;
    while (current != NULL) {
        next = current->next;
        if (list->free) {
            list->free(current->value);
        }
        zfree(current);
        current = next;
    }

    zfree(list);
}

/**
 * 将value添加到list的头部
 * 
 * @return list or NULL if no memory space
 */
List *listAddNodeHead(List *list, void *value) {
    ListNode *node = zmalloc(sizeof(ListNode));
    if (node == NULL) {
        return NULL;
    }
    node->value = value;
    if(list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
    return list;
}

/**
 * 将value添加到list的尾部
 * @return list or NULL if no memory space
 */
List *listAddNodeTail(List *list, void *value) {
    ListNode *node = zmalloc(sizeof(ListNode));
    if (node == NULL) {
        return NULL;
    }
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->next = NULL;
        list->tail->next = node;
        node->prev = list->tail;
        list->tail = node;
    }

    list->len++;
    return list;
}

/**
 * 从list中删除node
 * node一定要在list中
 */
void listDelNode(List *list, ListNode *node) {
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        list->head = node->next;
    }

    if (node->next != NULL) {
        node->next->prev = node->prev;
    } else {
        list->tail = node->prev;
    }

    if (list->free != NULL) {
        list->free(node->value);
    }

    zfree(node);
    list->len--;
}

/**
 * 获取list iterator, 调用listNextElement()会返回下一个元素
 */
ListIter *listGetIterator(List *list, int direction) {
    ListIter *iter = zmalloc(sizeof(ListIter));
    if (iter == NULL) {
        return NULL;
    }

    if (direction == AL_START_HEAD) {
        iter->next = list->head;
    } else {
        iter->next = list->tail;
    }

    iter->direction = direction;
    return iter;
}

void listReleaseIterator(ListIter *iter) {
    zfree(iter);
}

ListNode *listNextElement(ListIter *iter) {
    ListNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD) {
            iter->next = current->next;
        } else {
            iter->next = current->prev;
        }
    }

    return current;
}

/**
 * 拷贝list，如果list->dup方法设置了就是deep copy, 否则是sallow copy
 * 无论怎样，origin是不变的
 */
List *listDup(List *origin) {
    List *copy = listCreate();
    if (copy == NULL) {
        return NULL;
    }
    copy->dup = origin->dup;
    copy->free = origin->free;
    copy->match = origin->match;

    ListIter *iter = listGetIterator(origin, AL_START_HEAD);

    ListNode *node;
    while ((node = listNextElement(iter)) != NULL) {
        void *value = node->value;
        if (copy->dup != NULL) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                listReleaseIterator(iter);
                return NULL;
            }
        }

        if (listAddNodeTail(copy, value) == NULL) {
                listRelease(copy);
                listReleaseIterator(iter);
                return NULL;
        }
    }

    listReleaseIterator(iter);
    return copy;
}

/**
 * 找到key指定的node返回
 */
ListNode *listSearchKey(List *list, void *key) {
    ListIter *iter = listGetIterator(list, AL_START_HEAD);
    if (iter == NULL) {
        return NULL;
    }
    ListNode *node;
    while ((node = listNextElement(iter)) != NULL) {
        if(list->match != NULL) {
            if(list->match(node->value, key)) {
                break;
            }
        } else if (node->value == key){
            break;
        }
    }

    // 时刻注意，不要有内存泄露
    listReleaseIterator(iter);
    return node;
}

/**
 * 返回指定处的node
 * @param index index可以为负数，意味着从后往前数，比如index=-1表示最后一个元素
 */
ListNode *listIndex(List *list, int index) {
    ListNode *n;

    if (index < 0) {
        index = (-index) - 1;
        n = list->tail;
        while (index-- && n) {
            n = n->prev;
        }
    } else {
        n = list->head;
        while (index-- && n) {
            n = n->next;
        }
    }
    return n;
}

/************************* for debug *******************/
int *i_dup(int *v) {
    int *p = zmalloc(sizeof(int));
    *p = *v;
    return p;
}

void i_free(int *v) {
    zfree(v);
}

int i_match(int *v1, int *v2) {
    return *v1 == *v2;
}

void display(ListNode *node) {
    int *p = node->value;
    printf("%d\n", *p);
}

void printList(List* list, int direction) {
    ListIter *iter = listGetIterator(list, direction);
    ListNode *node;
    printf("[");
    if ((node = listNextElement(iter)) != NULL) {
        int *p = node->value;
        printf("%d", *p);
    }
    while((node = listNextElement(iter)) != NULL) {
        int *p = node->value;
        printf(", %d", *p);
    }
    printf("]\n");
    listReleaseIterator(iter);
}

int main() {
    List *list = listCreate();
    list->dup = i_dup;
    list->free = i_free;
    list->match = i_match;

    for (int i = 0; i < 10; i++) {
        int *p = zmalloc(sizeof(int));
        *p = i;
        listAddNodeTail(list, p);
    }

    printList(list, AL_START_HEAD);
    printf("------------------------------\n");

    for (int i = 0; i < 10; i++) {
        int *p = zmalloc(sizeof(int));
        *p = i;
        listAddNodeHead(list, p);
    }

    printList(list, AL_START_HEAD);
    printf("------------------------------\n");

    int key = 5;
    ListNode *node = listSearchKey(list, &key);
    display(node);
    listDelNode(list, node);
    printList(list, AL_START_HEAD);

    node = listSearchKey(list, &key);
    display(node);
    listDelNode(list, node);
    printList(list, AL_START_HEAD);

    List *copy = listDup(list);
    printList(copy, AL_START_HEAD);
    list->head->value = &key;
    printList(copy, AL_START_HEAD);
    printList(list, AL_START_HEAD);

    return 0;
}