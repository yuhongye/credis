#define REDIS_VERSION "0.07"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "ae.h"
#include "sds.h"
#include "anet.h"
#include "dict.h"
#include "adlist.h"
#include "zmalloc.h"

#define REDIS_OK 0
#define REDIS_ERR 1

/** static server configuration */
#define REDIS_SERVERPORT 6379
#define REDIS_MAXIDLETIME (60*5) // default client timeout
#define REDIS_QUERYBUF_LEN 1024
#define REDIS_LOADBUF_LEN 1024
#define REDIS_MAX_ARGS 16
#define REDIS_DEFAULT_DBNUM 16
#define REDIS_CONFIGLINE_MAX 1024
#define REDIS_OBJFREELIST_MAX 1000000 // max number of objects to cache, cache what?
#define REDIS_MAX_SYNC_TIME 60  // slave can't take more to sync

/** Hash table parameters */
#define REDIS_HT_MINFILL 10 // minimal hash table fill 10%
#define REDIS_HT_MINSLOTS 16384 // Never resize the HT under this

/** Command flags: 干嘛的? */
#define REDIS_CMD_BULK 1
#define REDIS_CMD_INLINE 2

/** Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_HASH 3
#define REDIS_SELECTDB 254
#define REDIS_EOF 255

/** Client flags */
#define REDIS_CLOSE 1 
#define REDIS_SLAVE 2
#define REDIS_MASTER 4

/** Server replication state */
#define REDIS_REPL_NONE 0    // no active replication
#define REDIS_REPL_CONNECT 1 // must connect to master
#define REDIS_REPL_CONNECTED 2 // connected to master

/** List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/** Sort operations */
#define REDIS_SORT_GET 0
#define REDIS_SORT_DEL 1
#define REDIS_SORT_INCR 2
#define REDIS_SORT_DECR 3
#define REDIS_SORT_ASC 4
#define REDIS_SORT_DESC 5
#define REDIS_SORTKEY_MAX 1024

/** Log levels */
#define REDIS_DEBUG 0
#define REDIS_NOTICE 1
#define REDIS_WARNING 2

/** 消除编译器warning */
#define REDIS_NOTUSED(V) ((void) V)

/*------------------- Data Types --------------------*/
/** A redis object, that is a type able to hold a string / list / set */
typedef struct RedisObject {
    int type;
    void *ptr;
    int refcount;
} Robj;

/**
 * with multiplexing we need to take per-client state
 * clients are taken in a liked list
 */
typedef struct RedisClient {
    int fd;
    Dict *dict;
    int dictid;
    sds querybuf;
    Robj *argv[REDIS_MAX_ARGS];
    int argc;
    int bulklen; // bulk read len, -1 if not in bulk read mode
    List *reply;
    int sentlen;
    time_t lastInteraction; // time of the last interaction, used for timeout
    int flags; // REDIS_CLOSE | REDIS_SLAVE
    int slaveSelDb; // slave selected db, if this client is a slave
} RedisClient;

struct SaveParam {
    time_t seconds;
    int changes;
};

/**
 * Global server state structure
 */
struct RedisServer {
    int port;
    int fd;
    Dict **dict;
    long long dirty; // changes to db from the last save
    List *clients;
    List *slaves;
    char neterr[ANET_ERR_LEN];
    AeEventLoop *el;
    int cronloops; // number of times the cron function run
    List *objFreeList; // A list of freed objects to avoid malloc()
    time_t lastsave; // unix time of last save successed
    int usedmemory; // used memory in megabytes
    
    /** 统计字段 */
    time_t stat_starttime;  // server start time
    long long stat_numcommands;  // number of processed commands
    long long stat_numconnections; // number of connections received

    /** 配置 */
    int verbosity;
    int glueOutputBuf;
    int maxIdleTime;
    int dbnum;
    int daemonize;
    int bgsaveInProgress;
    struct SaveParam *saveParams;
    int saveParamLens;
    char *logfile;
    char *bindaddr;
    char *dbfilename;

    /** Replication related */
    int isslave;
    char *masterhost;
    int masterport;
    RedisClient *master;
    int replState;

    /** sort parameters */
    int sort_desc;
    int sort_alpha;
    int sort_byPattern;
};

typedef void redisCommandProc(RedisClient *c);
struct RedisCommand {
    char *name;
    redisCommandProc *proc;
    int arity; // 参数个数
    int flags;
};

typedef struct _RedisSortObject {
    Robj *obj;
    union {
        double score;
        Robj *cmpObj;
    } u;
} RedisSortedObject;

typedef struct _RedisSortOperation {
    int type;
    Robj *pattern;
} RedisSortOperation;

/**
 * 用一个结构体把一些常量包起来？
 */
struct SharedObjectStruct {
    Robj *crlf, *ok, *err, *zerobulk, *nil, *zero, *one, *pong, *space,
    *minus1, *minus2, *minus3, *minus4,
    *wrongTypeErr, *noKeyErr, *wrongTypeErrBulk, *noKeyErrBulk,
    *syntaxErr, *syntaxErrBulk,
    *select0, *select1, *select2, *select3, *select4, 
    *select5, *select6, *select7, *select8, *select9;
} shared; // 这种语法是创建了一个实例，名字叫shared

/**------------------------- Prototypes -------------------*/
static void freeStringObject(Robj *o);
static void freeListObject(Robj *o);
static void freeSetObject(Robj *o);
static void descRefCount(void *o);
static Robj *createObject(int type, void *ptr);
static void freeClient(RedisClient *c);
static int loadDb(char *filename);
static void addReply(RedisClient *c, Robj *obj);
static void addReplaySds(RedisClient *c, sds s);
static void incrRefCount(Robj *o);
static int saveDbBackground(char *filename);
static Robj *createStringObject(char *ptr, size_t len);
static void replicationFeedSlaves(struct RedisCommand *cmd, int dictid, Robj **argv, int argc);
static int syncWithMaster(void);

static void pingCommand(RedisClient *c);
static void echoCommand(RedisClient *c);
static void setCommand(RedisClient *c);
static void setnxCommand(RedisClient *c);
static void getCommand(RedisClient *c);
static void delCommand(RedisClient *c);
static void existsComand(RedisClient *c);
static void incrCommand(RedisClient *c);
static void decrCommand(RedisClient *c);
static void selectCommand(RedisClient *c);
static void randomkeyCommand(RedisClient *c);
static void keysCommand(RedisClient *c);
static void dbsizeCommand(RedisClient *c);
static void lastsaveCommand(RedisClient *c);
static void saveCommand(RedisClient *c);
static void bgsaveCommand(RedisClient *c);
static void shutdownCommand(RedisClient *c);
static void moveCommand(RedisClient *c); // move what
static void renameCommand(RedisClient *c);
static void renamenxCommand(RedisClient *c);
static void lpushCommand(RedisClient *c);
static void rpushCommand(RedisClient *c);
static void lpopCommand(RedisClient *c);
static void rpopCommand(RedisClient *c);
static void llenCommand(RedisClient *c);
static void lindexCommand(RedisClient *c);
static void lrangeCommand(RedisClient *c);
static void ltrimCommand(RedisClient *c);
static void typeCommand(RedisClient *c);
static void lsetCommand(RedisClient *c);
static void saddCommand(RedisClient *c);
static void sremCommand(RedisClient *c);
static void sismemberCommand(RedisClient *c);
static void scardCommand(RedisClient *c);
static void sinterCommand(RedisClient *c);
static void sinterstoreCommand(RedisClient *c);
static void syncCommand(RedisClient *c);
static void flushdbCommand(RedisClient *c);
static void flushallCommand(RedisClient *c);
static void sortCommand(RedisClient *c);
static void lremCommand(RedisClient *c);
static void infoCommand(RedisClient *c);

/** --------------------------- Globals -------------------------------- */
static struct RedisServer server;
static struct RedisCommand cmdTable[] = {
    {"get", getCommand, 2, REDIS_CMD_INLINE},
    {"set", setCommand, 3, REDIS_CMD_BULK},
    {"setnx", setnxCommand, 3, REDIS_CMD_BULK},
    {"del", delCommand, 2, REDIS_CMD_INLINE},
    {"exists", existsComand, 2, REDIS_CMD_INLINE},
    {"incr", incrCommand, 2, REDIS_CMD_INLINE},
    {"decr", decrCommand, 2, REDIS_CMD_INLINE},
    {"rpush", rpushCommand, 3, REDIS_CMD_BULK},
    {"lpush", lpushCommand, 3, REDIS_CMD_BULK},
    {"rpop", rpopCommand, 2, REDIS_CMD_INLINE},
    {"lpop", lpopCommand, 2, REDIS_CMD_INLINE},
    {"llen", llenCommand, 2, REDIS_CMD_INLINE},
    {"lindex", lindexCommand, 3, REDIS_CMD_INLINE},
    {"lset", lsetCommand, 4, REDIS_CMD_BULK},
    {"lrange", lrangeCommand, 4, REDIS_CMD_INLINE},
    {"ltrim", ltrimCommand, 4, REDIS_CMD_INLINE},
    {"lrem", lremCommand, 4, REDIS_CMD_BULK},
    {"sadd", saddCommand, 3, REDIS_CMD_BULK},
    {"srem", sremCommand, 3, REDIS_CMD_BULK},
    {"sismember", sismemberCommand, 3, REDIS_CMD_BULK},
    {"scard", scardCommand, 2, REDIS_CMD_INLINE}
};

/*-------------------- 工具函数 ------------------*/
int stringMatchLen(const char *pattern, int patternLen, const char *string, int stringLen, int nocase) {
    // TODO: 补充完整
    if (patternLen == 0 && stringLen == 0) {
        return 1;
    }
    return 0;
}

/**
 * 是没来一条log都要进行一次file open/close吗？
 */
void redisLog(int level, const char *fmt, ...) {
    FILE *fp = (server.logfile == NULL) ? stdout : fopen(server.logfile, "a");
    if (fp == NULL) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    if (level >= server.verbosity) {
        char *c = ".-*";
        fprintf(fp, "%c ", c[level]);
        vfprintf(fp, fmt, ap);
        fprintf(fp, "\n");
        fflush(fp);
    }
    va_end(ap);

    if (server.logfile != NULL) {
        fclose(fp);
    }
}

/*------------------- Hash table type implementation -------------------*/
/** key是sds，value是redis objects*/

static int sdsDictKeyCompare(void *privdata, const void *key1, const void *key2) {
    DICT_NOUSED(privdata);
    int l1 = sdslen((sds) key1);
    int l2 = sdslen((sds) key2);
    if (l1 != l2) {
        return 0;
    }

    return memcmp(key1, key2, l1) == 0;
}

static void dictRedisObjectDestructor(void *privdata, void *val) {
    DICT_NOUSED(privdata);

    decrRefCount(val);
}

/** 
 * key也会包装成Robj吗？ 
 */
static int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2) {
    const Robj *o1 = key1;
    const Robj *o2 = key2;
    return sdsDictKeyCompare(privdata, o1->ptr, o2->ptr);
}

static unsigned int dictSdsHash(const void *key) {
    const Robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds) o->ptr));
}

static DictType setDictType = {
    dictSdsHash, // hash function
    NULL,        // key dup
    NULL,        // value dup
    dictSdsKeyCompare, // key compare
    dictRedisObjectDestructor, // key destructor
    NULL                       // value destructor
};

static DictType hashDictType = {
    dictSdsHash, // hash function
    NULL, // key dup
    NULL, // key dup
    dictSdsKeyCompare, // key compare
    dictRedisObjectDestructor, // key destructor
    dictRedisObjectDestructor, // value destructor
};

/*------------------------------ random utility functions ---------------------*/
/**
 * 通常情况下resi是不会cover oom这种异常，由于发送给client的数据的send buffer也依赖heap，
 * 因此oom时还能不能通知client都是不清楚的，因此我们只是让错误信息更直观理解
 */
static void oom(const char *msg) {
    fprintf(stderr, "%s: Out Of Memory\n", msg);
    fflush(stderr);
    sleep(1);
    abort();
}

/*-------------------- Redis server networking stuff ----------------------*/
void closeTimeoutClients(void) {
    ListIter *it = listGetIterator(server.clients, AL_START_HEAD);
    if (it == NULL) {
        return;
    }

    ListNode *node;
    time_t now = time(NULL);
    while ((node = listNextElement(it)) != NULL) {
        RedisClient *c = listNodeValue(node);
        // slave没有timeout
        if (!(c->flags & REDIS_SLAVE) && (now - c->lastInteraction) > server.maxIdleTime) {
            redisLog(REDIS_DEBUG, "Closing idle client");
            freeClient(c);
        }
    }
    listReleaseIterator(it);
}

/** 如果HT的使用量超过REDIS_HT_MINFILL, rehash以节省内存空间; 
 * TODO: rehash怎么节省内存空间了，是减少了链表吗 
 */
static void rehashIfNeed(int loops) {
   for (int j = 0; j< server.dbnum; j++) {
        int size = dictGetHashTableSize(server.dict[j]);
        int used = dictGetHashTableUsed(server.dict[j]);
        if ((loops % 5 == 0) && used > 0) {
            redisLog(REDIS_DEBUG, "DB %d: %d keys in %d slots HT", j, used, size);
        }
        if (size > 0 && used > 0 && size > REDIS_HT_MINSLOTS && (used*100/size < REDIS_HT_MINFILL)) {
            redisLog(REDIS_NOTICE, "The hash table %d is too sparse, resize it...", j);
            dictResize(server.dict[j]);
            redisLog(REDIS_NOTICE, "Hash table %d resized.", j);
        }
    } 
}

/**
 * 等待正在进行的bgsave完整，并更新server中跟bgsave相关的参数
 */
static void waitBgsaveFinish() {
    int statloc;
    // 等待直到指定的子pid状态改变，这不是posix的接口，不过所有系统都提供
    if (wait4(-1, &statloc, WNOHANG, NULL)) {
        int exitcode = WEXITSTATUS(statloc);
        if (exitcode == 0) {
            redisLog(REDIS_NOTICE, "Background saving terminated with success");
            server.dirty = 0;
            server.lastsave = time(NULL);
        } else {
            redisLog(REDIS_WARNING, "Background saving error");
        }
        server.bgsaveInProgress = 0;
    }
}

/**
 * 如果修改次数和时间符合，则启动一个新的bgsave
 */
static void startNewBgsaveIfNeed() {
    // 看看是否有必要启动一次bgsave
    time_t now = time(NULL);
    for (int i = 0; i < server.saveParamLens; i++) {
        struct SaveParam *sp = server.saveParams + i;
        // 修改达到次数，且时间达到
        if (server.dirty >= sp->changes && (now-server.lastsave) > sp->seconds) {
            redisLog(REDIS_NOTICE, "%d changed in %d seconds, Saveing...", sp->changes, sp->seconds);
            saveDbBackground(server.dbfilename);
            break;
        }
    }
}

/**
 *  1. 如果当前正在bgsave，则等它成功，否则
 *  2. 如果修改次数和时间达到要求，则启动一个新的bgsave
 */
static void waitBgsaveOrStartNewIfNeed() {
    // 是否有background saving
    if (server.bgsaveInProgress) {
        waitBgsaveFinish();
    } else {
        startNewBgsaveIfNeed();
    }
}

/**
 * server端的定时调用？
 * 1. rehash
 * 2. 打印client信息
 * 3. 关闭超时client
 * 4. bgsave
 * 5. sync with master if its replicator
 * @return 1000是什么意思？
 */
int serverCron(struct AeEventLoop *eventLoop, long long id, void *clientData) {
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    // 更新全局memory used
    server.usedmemory = zmalloc_used_memory();
    int loops = server.cronloops;

    rehashIfNeed(loops);

    // 打印连接的client的信息
    if (loops % 5 == 0) {
        redisLog(REDIS_DEBUG, "%d clients connected(%d slaves), %d bytes in use", 
            listLength(server.clients) - listLength(server.slaves), listLength(server.slaves), server.usedmemory);
    }

    // 每10次去关闭已经超时的client
    if (loops % 10 == 0) {
        closeTimeoutClients();
    }

    // 处理bgsave
    waitBgsaveOrStartNewIfNeed();
    
    if (server.replState == REDIS_REPL_CONNECT) {
        redisLog(REDIS_NOTICE, "Connecting to MASTER...");
        if (syncWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync succeeded.");
        }
    }

    // why return 1000;
    return 1000;
}


static Robj *createObjectUseString(char *v) {
    return createObject(REDIS_STRING, sdsnew(v));
}
static void createShareObjects(void) {
    shared.crlf = createObjectUseString("\r\n");
    shared.ok = createObjectUseString("+OK\r\n");
    shared.err = createObjectUseString("-ERR\r\n");
    shared.zerobulk = createObjectUseString("0\r\n\r\n");
    shared.nil = createObjectUseString("nil\r\n");
    shared.zero = createObjectUseString("0\r\n");
    shared.one = createObjectUseString("1\r\n");
    shared.space = createObjectUseString(" ");
    // no such key
    shared.minus1 = createObjectUseString("-1\r\n");
    // operation against key holding a value of the wrong type
    shared.minus2 = createObjectUseString("-2\r\n");
    // src and dest are the same
    shared.minus3 = createObjectUseString("-3\r\n");
    // out of range argument
    shared.minus4 = createObjectUseString("-4\r\n");
    shared.pong = createObjectUseString("+PONG\r\n");
    shared.wrongTypeErr = createObjectUseString("-ERR Operation against a key holding the wrong kind of value\r\n");
    shared.wrongTypeErrBulk = createObject(REDIS_STRING, sdscatprintf(sdsempty(), "%d\r\n%s", 
                                -sdslen(shared.wrongTypeErr->ptr) + 2, shared.wrongTypeErr->ptr));
    shared.noKeyErr = createObjectUseString("-ERR no suck key\r\n");
    shared.noKeyErrBulk = createObject(REDIS_STRING, sdscatprintf(sdsempty(), "%d\r\n%s", 
                            -sdslen(shared.noKeyErr->ptr) + 2, shared.noKeyErr->ptr));
    shared.syntaxErr = createObjectUseString("-ERR syntax error\r\n");
    shared.syntaxErrBulk = createObject(REDIS_STRING, sdscatprintf(sdsempty(), "%d\r\n%s", 
                            -sdslen(shared.syntaxErr->ptr) + 2, shared.syntaxErr->ptr));
    
    shared.select0 = createStringObject("select 0\r\n", 10);
    shared.select1 = createStringObject("select 1\r\n", 10);
    shared.select2 = createStringObject("select 2\r\n", 10);
    shared.select3 = createStringObject("select 3\r\n", 10);
    shared.select4 = createStringObject("select 4\r\n", 10);
    shared.select5 = createStringObject("select 5\r\n", 10);
    shared.select6 = createStringObject("select 6\r\n", 10);
    shared.select7 = createStringObject("select 7\r\n", 10);
    shared.select8 = createStringObject("select 8\r\n", 10);
    shared.select9 = createStringObject("select 9\r\n", 10);
}

static void appendServerSaveParams(time_t seconds, int changes) {
    server.saveParams = zrealloc(server.saveParams, sizeof(struct SaveParam) * (server.saveParamLens+1));
    if (server.saveParams == NULL) {
        oom("appendServerSaveParams");
    }
    server.saveParams[server.saveParamLens].seconds = seconds;
    server.saveParams[server.saveParamLens].changes = changes;
    server.saveParamLens++;
}

static void resetServerSaveParams() {
    zfree(server.saveParams);
    server.saveParams == NULL;
    server.saveParamLens = 0;
}

static void initServerConfig() {
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.port = REDIS_SERVERPORT;
    server.verbosity = REDIS_DEBUG;
    server.masterport = REDIS_MAXIDLETIME;
    server.saveParams = NULL;
    server.logfile = NULL; // use standard output
    server.bindaddr = NULL;
    server.glueOutputBuf = 1;
    server.daemonize = 0;
    server.dbfilename = "dump.rdb";
    resetServerSaveParams();

    // 梯次配置save rdb时机
    appendServerSaveParams(60*60, 1); // 一小时中只要有一次就save
    appendServerSaveParams(5*60, 100); // 5分钟100次
    appendServerSaveParams(60, 10000); // 1分钟10000次

    server.isslave = 0;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.replState = REDIS_REPL_NONE;
}

static void initServer() {
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    server.clients = listCreate();
    server.slaves = listCreate();
    server.objFreeList = listCreate();
    createShareObjects();
    server.el = aeCreateEventLoop();
    server.dict = zmalloc(sizeof(Dict*) * server.dbnum);
    if (server.dict == NULL || server.clients == NULL || server.slaves == NULL || server.objFreeList == NULL) {
        oom("server initialization");
    }
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "Openning TCP port: %s", server.neterr);
        exit(1);
    }

    // 创建每个db保存数据的ht
    for (int i = 0; i < server.dbnum; i++) {
        server.dict[i] = dictCreate(&hashDictType, NULL);
        if (server.dict[i] == NULL) {
            oom("dictCreate");
        }
    }

    server.cronloops = 0;
    server.bgsaveInProgress = 0;
    server.lastsave = time(NULL);
    server.dirty = 0;
    server.usedmemory = 0;
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_starttime = time(NULL);

    // 创建server端的定时任务，用来处理过期的client, 进行bgsave, rehash db的ht等
    aeCreateTimeEvent(server.el, 1000, serverCron, NULL, NULL);
}

/**
 * 清空整个redis的数据
 */
static void emptyDb() {
    for (int i = 0; i < server.dbnum; i++) {
        dictEmpty(server.dict[i]);
    }
}

/**
 * 一个比较初级的用来从文件中读取配置的实现
 */
static void loadServerConfig(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        redisLog(REDIS_WARNING, "Fatal error, can't open config file");
        exit(1);
    }

    char buf[REDIS_CONFIGLINE_MAX+1];
    char *err = NULL;
    int linenum = 0;
    sds line = NULL;
    while (fget(buf, REDIS_CONFIGLINE_MAX+1, fp) != NULL) {
        linenum++;
        line = sdsnew(buf);
        line = sdstrim(line, "\t\r\n");
        if (line[0] == '#' || line[0] == '\0') {
            sdsfree(line);
            continue;
        }

        int argc;
        sds *argv = sdssplitlen(line, sdslen(line), " ", 1, &argc);
        sdstolower(argv[0]);

        if (strcmp(argv[0], "timeout") && argc == 2) {
            server.maxIdleTime = atoi(argv[1]);
            if (server.maxIdleTime < 1) {
                err = "Invalid timeout value";
                goto loaderr;
            }
        } else if (strcmp(argv[0], "port") == 0 && argc == 2) {
            server.port = atoi(argv[1]);
            if (server.port < 1 || server.port > 65535) {
                err = "Invalid port";
                goto loaderr;
            } 
        } else if (strcmp(argv[0], "bind") == 0 && argc == 2) {
            server.bindaddr = zstrdup(argv[1]);
        } else if (strcmp(argv[0], "save") == 0 && argc == 3) {
            int seconds = atoi(argv[1]);
            int changes = atoi(argv[2]);
            if (seconds < 1 || changes < 0) {
                err = "Invalid save paramaters";
                goto loaderr;
            }
            appendServerSaveParams(seconds, changes);
        } else if (strcmp(argv[0], "dir") && argc == 2) {
            if (chdir(argv[1]) == -1) {
                redisLog(REDIS_WARNING, "Can't chdir to '%s': '%s'", argv[1], strerror(errno));
                exit(1);
            }
        } else if (strcmp(argv[0], "loglevel") && argc == 2) {
            if (strcmp(argv[1], "debug") == 0) {
                server.verbosity = REDIS_DEBUG;
            } else if (strcmp(argv[1], "notice") == 0) {
                server.verbosity = REDIS_NOTICE;
            } else if (strcmp(argv[1], "warning") == 0) {
                server.verbosity = REDIS_WARNING;
            } else {
                err = "Invalid log level. Must be one of debug, notice, warning";
                goto loaderr;
            }
        } else if (strcmp(argv[0], "logfile") == 0 && argc == 2) {
            server.logfile = zstrdup(argv[1]);
            if (strcmp(server.logfile, "stdout") == 0) {
                zfree(server.logfile);
                server.logfile = NULL;
            }
            if (server.logfile != NULL) {
                FILE *fp = fopen(server.logfile, "a");
                if (fp == NULL) {
                    err = sdscatprintf(sdsempty(), "Can't open the log file: %s", strerror(errno));
                    goto loaderr;
                }
                fclose(fp);
            }
        } else if (strcmp(argv[0], "databases") == 0 && argc == 2) {
            server.dbnum = atoi(argv[1]);
            if (server.dbnum < 1) {
                err = "Invalid number of databases";
                goto loaderr;
            }
        } else if (strcmp(argv[0], "slaveof") == 0 && argc == 3) {
            server.masterhost = sdsnew(argv[1]);
            server.masterport = atoi(argv[2]);
            server.replState = REDIS_REPL_CONNECT;
        } else if (strcmp(argv[0], "glueoutputbuf" && argc == 2)) {
            sdstolower(argv[1]);
            if (strcmp(argv[1], "yes") == 0) {
                server.glueOutputBuf = 1;
            } else if (strcmp(argv[1], "no") == 0) {
                server.glueOutputBuf = 0;
            } else {
                err = "argument must be 'yes' or 'no'";
                goto loaderr;
            }
        } else if (strcmp(argv[0], "daemonize") == 0 && argc == 0) {
            sdstolower(argv[1]);
            if (strcmp(argv[1], "yes") == 0) {
                server.daemonize = 1;
            } else if (strcmp(argv[1], "no") == 0) {
                server.daemonize = 0;
            } else {
                err = "argument must be 'yes' or 'no'";
                goto loaderr;
            }
        } else {
            err = "Bad directive or wrong number of arguments";
            goto loaderr;
        }

        for (int i = 0; i < argc; i++) {
            sdsfree(argv[i]);
        }
        zfree(argv);
        sdsfree(line);
    }

    loaderr: 
        fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
        fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
        fprintf(stderr, ">>> '%s'\n", line);
        fprintf(stderr, "%s\n", err);
        exit(1);
}

int main(int argc, char **argv) {
    initServerConfig();
}
