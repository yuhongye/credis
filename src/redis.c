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

struct SharedObjectStruct {
    Robj *ctrl, *ok, *err, *zerobulk, *nil, *zero, *one, *pong, *space,
    *minus1, *minus2, *minus3, *minus4,
    *wrongTypeErr, *noKeyErr, *wrongTypeErrBulk, *noKeyErrBulk,
    *syntaxErr, *syntaxErrBulk,
    *select0, *select1, *select2, *select3, *select4, 
    *select5, *select6, *select7, *select8, *select9;
} Shared;

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
static struct RedisServer sever;
static struct RedisCommand cmdTable[] = {
    {"get", getCommand, 2, }
};