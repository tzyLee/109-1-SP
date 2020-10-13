
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ERR_EXIT(a) \
    do {            \
        perror(a);  \
        exit(1);    \
    } while (0)
#define writeStr(fd, str) write((fd), (str), strlen((str)))

// manage file descriptors to poll
struct pollfd* pollFds;
int pollFdsLen;
static void appendFd(int fd);
static void removeFd(int fd);
//

// Server
typedef struct {
    char hostname[512];   // server's hostname
    unsigned short port;  // port to listen
    int listen_fd;        // fd to wait for a new connection
} Server;
Server svr;  // server
char buf[512];
struct sockaddr_in cliAddr;  // used by accept()
int cliLen = sizeof(cliAddr);
int maxFd;  // size of open file descriptor table, size of request list
int recordFd;
static void initServer(unsigned short port);
static int acceptAndBlock();
//

// Request
typedef struct {
    char host[512];  // client's host
    int conn_fd;     // fd to talk with client
    char buf[512];   // data sent by/to client
    size_t buf_len;  // bytes used by buf
    // you don't need to change this.
    int id;
    int nextActionId;  // used by handle_read to know if the header is read or
                       // not.
} Request;
Request* requests = NULL;  // point to a list of requests
//

// IO
typedef enum { UNLCK, RDLCK, WRLCK } LockStat;  // UNLCK == 0, easier to memset
typedef struct {
    LockStat status;
    Request* owner;
} LockInfo;
typedef struct {
    int id;  // 902001-902020, customer id, set to negative if this process
             // have write lock on the section
    int adultMask;     // set to 10 by default
    int childrenMask;  // set to 10 by default
} Order;
Order* idInfo = NULL;  // only used to find id
LockInfo* lockInfo =
    NULL;  // The type of lock acquired by this process at the offset
int idInfoLen;
static Order orderBuf;
static int initializeIdInfo();
static ssize_t safeRead(int fd, char* buf, size_t count);
static int acquireLock(int fd, short type, short whence, off_t offset,
                       off_t len);
static int acquireLockBlocking(int fd, short type, short whence, off_t offset,
                               off_t len);
static int tryAcquireReadLock(Request* req, int idx);
static int tryAcquireWriteLock(Request* req, int idx);
static int releaseLock(Request* req, int idx);
static int getIndexOfId(int id);
static off_t getFileSize(int fd);
//

// Action
typedef enum { SUCCESS, FAILED, LOCKED } Result;
typedef Result (*RequestHandler)(Request*);
typedef struct {
    const char* prompt;
    RequestHandler handler;
} Action;
static void initRequest(Request*);
static void cleanUpRequest(Request*);
static int readRequest(Request* req);
static Result startRequest(Request*);
static Result lookUpRecord(Request*);
static Result handleOrder(Request*);
//

// action taken when a file is ready to be read
Action actions[] = {
    {.prompt = NULL, .handler = startRequest},
    {.prompt = "Please enter the id (to check how many masks can be ordered):",
     .handler = lookUpRecord},
#ifndef READ_SERVER
    {.prompt = "Please enter the mask type (adult or children) and number of "
               "mask you would like to order:",
     .handler = handleOrder},
#endif
};

int actionsLen = sizeof(actions) / sizeof(actions[0]);

int main(int argc, char* argv[]) {
    // Parse args.
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        exit(1);
    }

    initializeIdInfo();

    // Initialize server
    initServer((unsigned short)atoi(argv[1]));

    // Loop for handling connections
    fprintf(stderr, "\nstarting on %.80s, port %d, fd %d, maxconn %d...\n",
            svr.hostname, svr.port, svr.listen_fd, maxFd);

    while (1) {
#ifndef NDEBUG
        fprintf(stderr, "Now polling: (");
        for (int i = 0; i < pollFdsLen; ++i) {
            fprintf(stderr, "%d, ", pollFds[i].fd);
        }
        fprintf(stderr, ")\n");
        fprintf(stderr, "<<< start polling ......");
#endif
        int totalFd = poll(pollFds, pollFdsLen, -1);
#ifndef NDEBUG
        fprintf(stderr, "end polling, %d ready >>>\n", totalFd);
#endif
        // timeout == -1, totalFd != 0
        if (totalFd < 0) {
            ERR_EXIT("poll");
        }
        for (int i = 0; i < pollFdsLen;) {
            if (pollFds[i].revents == 0) {
                ++i;
                continue;
            }
            int fd = pollFds[i].fd;
            if (i == 0) {
                // Check for new connection
                fd = acceptAndBlock();
                initRequest(&requests[fd]);
                requests[fd].conn_fd = fd;
            }
            int nextActionId = requests[fd].nextActionId;
            // if (nextActionId == actionsLen)
            Result res = actions[nextActionId].handler(&requests[fd]);
            switch (res) {
            case SUCCESS:
                if (++nextActionId == actionsLen) {
                    cleanUpRequest(&requests[fd]);
                } else {
                    requests[fd].nextActionId = nextActionId;
                    // send next prompt
                    if (actions[nextActionId].prompt) {
                        writeStr(fd, actions[nextActionId].prompt);
                    }
                }
                break;
            case FAILED:
                writeStr(fd, "Operation failed.\n");
                cleanUpRequest(&requests[fd]);
                break;
            case LOCKED:
                writeStr(fd, "Locked.\n");
                cleanUpRequest(&requests[fd]);
                break;
            }
            if (--totalFd == 0)
                break;
        }
    }
    free(requests);
    free(pollFds);
    free(idInfo);
    free(lockInfo);
    requests = NULL;
    pollFds = NULL;
    idInfo = NULL;
    lockInfo = NULL;
    close(recordFd);
    return 0;
}

static int acceptAndBlock() {
    int conn_fd = 0;
    do {
        conn_fd = accept(svr.listen_fd, (struct sockaddr*)&cliAddr,
                         (socklen_t*)&cliLen);
        // success
        if (conn_fd >= 0)
            break;
        // error
        switch (errno) {
        case EINTR:
        case EAGAIN:
            continue;  // try again
        case ENFILE:
            (void)fprintf(stderr,
                          "out of file descriptor table ... (maxconn %d)\n",
                          maxFd);
            return -1;
        default:
            ERR_EXIT("accept");
        }
    } while (1);
    return conn_fd;
}

static void initServer(unsigned short port) {
    struct sockaddr_in servaddr;
    int tmp;

    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;

    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0)
        ERR_EXIT("socket");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    tmp = 1;
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp,
                   sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    if (bind(svr.listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) <
        0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }

    // Get file descripter table size and initize request table
    maxFd = getdtablesize();
    requests = (Request*)malloc(sizeof(Request) * maxFd);
    if (requests == NULL) {
        ERR_EXIT("out of memory allocating all requests");
    }
    for (int i = 0; i < maxFd; i++) {
        initRequest(&requests[i]);
    }
    requests[svr.listen_fd].conn_fd = svr.listen_fd;
    strcpy(requests[svr.listen_fd].host, svr.hostname);

    pollFds = (struct pollfd*)malloc(sizeof(struct pollfd) * maxFd);
    pollFdsLen = 0;
    appendFd(svr.listen_fd);
    return;
}

static void appendFd(int fd) {
    // assume 0 <= fds_len < maxfds;
    pollFds[pollFdsLen].fd = fd;
    pollFds[pollFdsLen].events = POLLIN;  // can be read
    pollFds[pollFdsLen].revents = 0;
    ++pollFdsLen;
}

static void removeFd(int fd) {
    // assume 0 <= pollFdsLen < maxFds;
    int idx = 0;
    for (; idx < pollFdsLen; ++idx) {
        if (pollFds[idx].fd == fd)
            break;
    }
    if (idx == pollFdsLen)
        // Not found
        return;

    if (idx != pollFdsLen - 1) {
        // move pollFdsLen[-1] to pollFdsLen[idx]
        memcpy(pollFds + idx, pollFds + pollFdsLen - 1, sizeof(struct pollfd));
    }
    --pollFdsLen;
}

static void initRequest(Request* req) {
    req->conn_fd = -1;
    req->buf_len = 0;
    req->id = -1;
    req->nextActionId = 0;
}

static void cleanUpRequest(Request* req) {
#ifndef NDEBUG
    fprintf(stderr, "close %d\n", req->conn_fd);
#endif
    close(req->conn_fd);     // close connection
    removeFd(req->conn_fd);  // remove from pollFds
    initRequest(req);        // reset req
}

static int readRequest(Request* req) {
    static char buf[512];
    req->buf_len = 512;
    int nRead = read(req->conn_fd, buf, sizeof(buf));
    if (nRead < 512) {
        buf[nRead] = '\0';
        req->buf_len = nRead;
    }
    strncpy(req->buf, buf, 512);
#ifndef NDEBUG
    fprintf(stderr, "received raw: '");
    for (char *c = req->buf, *end = req->buf + req->buf_len; *c && c != end;
         ++c) {
        if (isprint(*c))
            fputc(*c, stderr);
        else
            switch (*c) {
            case '\n':
                fprintf(stderr, "\\n");
                break;
            case '\r':
                fprintf(stderr, "\\r");
                break;
            default:
                fprintf(stderr, "\\x%02x", *c);
                break;
            }
    }
    fprintf(stderr, "', from fd %d, buf_len is %ld\n", req->conn_fd,
            req->buf_len);
#endif
    return nRead;
}

static Result startRequest(Request* req) {
    strcpy(req->host, inet_ntoa(cliAddr.sin_addr));
    appendFd(req->conn_fd);  // add to pollFds
#ifndef NDEBUG
    fprintf(stderr, "getting a new request... fd %d from %s\n", req->conn_fd,
            req->host);
#endif
    return SUCCESS;
}

static Result lookUpRecord(Request* req) {
    if (readRequest(req) <= 0) {
        // 0: disconnect
        // -1: error
        return FAILED;
    }
    // Error: resource temporarily unavailable
    char* firstInvalid;
    errno = 0;
    int id = strtol(req->buf, &firstInvalid, 10);
    if (req->buf == firstInvalid || errno != 0) {
#ifndef NDEBUG
        fprintf(stderr, "invalid number, '%s'\n", req->buf);
#endif
        return FAILED;
    }
#ifndef NDEBUG
    fprintf(stderr, "id is %d\n", id);
#endif

    req->id = id;
    int idx = getIndexOfId(id);
    if (idx < 0)
        // ID not found
        return FAILED;

    if (tryAcquireReadLock(req, idx) < 0)
        return LOCKED;

    lseek(recordFd, idx * sizeof(Order), SEEK_SET);
    if (safeRead(recordFd, (char*)&orderBuf, sizeof(Order)) < 0) {
        releaseLock(req, idx);
        return FAILED;
    }

    if (releaseLock(req, idx) < 0)
        return FAILED;

#ifndef READ_SERVER
    if (tryAcquireWriteLock(req, idx) < 0)
        return LOCKED;
#endif
    idInfo[idx] = orderBuf;  // The record is not locked now

    sprintf(buf, "You can order %d adult mask(s) and %d children mask(s).\n",
            idInfo[idx].adultMask, idInfo[idx].childrenMask);
    write(req->conn_fd, buf, strlen(buf));
    return SUCCESS;
}

static Result handleOrder(Request* req) {
    int idx = getIndexOfId(req->id);
    if (idx < 0)
        // ID not found
        return FAILED;

    if (readRequest(req) <= 0) {
        // 0: disconnect
        // -1: error
        releaseLock(req, idx);
        return FAILED;
    }

    int orderChild = 0;
    int orderAdult = 0;
    if (sscanf(req->buf, "adult %d", &orderAdult) != 1 &&
        sscanf(req->buf, "children %d", &orderChild) != 1) {
        // no valid command
        releaseLock(req, idx);
        return FAILED;
    }
#ifndef NDEBUG
    fprintf(stderr, "Ordered adult: %d, children: %d\n", orderAdult,
            orderChild);
#endif

    // The lock is owned by another request in this process
    if (lockInfo[idx].owner != req) {
#ifndef NDEBUG
        fprintf(stderr, "owner is %p, current request is %p\n",
                lockInfo[idx].owner, req);
#endif
        return LOCKED;
    }

#ifndef NDEBUG
    fprintf(stderr, "%d/%d, %d/%d\n", orderAdult, idInfo[idx].adultMask,
            orderChild, idInfo[idx].childrenMask);
#endif

    if (orderAdult > idInfo[idx].adultMask ||
        orderChild > idInfo[idx].childrenMask ||
        (orderAdult <= 0 && orderChild <= 0)) {
        releaseLock(req, idx);
        // number not in range: negative / zero / too large
        return FAILED;
    }

    orderBuf.id = req->id;
    orderBuf.adultMask = idInfo[idx].adultMask - orderAdult;
    orderBuf.childrenMask = idInfo[idx].childrenMask - orderChild;

    lseek(recordFd, idx * sizeof(Order), SEEK_SET);
    do {
        int ret = write(recordFd, &orderBuf, sizeof(Order));
        if (ret == sizeof(Order))
            break;
        if (errno != EINTR) {
            releaseLock(req, idx);
            return FAILED;
        }
    } while (1);

    if (releaseLock(req, idx) < 0)
        return FAILED;

    if (orderAdult) {
        sprintf(buf, "Pre-order for %d succeeded, %d adult mask(s) ordered.\n",
                req->id, orderAdult);

    } else {
        sprintf(buf,
                "Pre-order for %d succeeded, %d children mask(s) ordered.\n",
                req->id, orderChild);
    }
    write(req->conn_fd, buf, strlen(buf));
    return SUCCESS;
}

static int initializeIdInfo() {
    do {
        recordFd = open("./preorderRecord", O_RDWR);
        if (recordFd >= 0)
            break;
        if (errno != EINTR)
            return -1;
    } while (1);

    if (recordFd < 0)
        ERR_EXIT("failed to open ./preorderRecord");
    // acquire read lock on whole file
    acquireLockBlocking(recordFd, F_RDLCK, SEEK_SET, 0, 0);
    size_t nBytes = getFileSize(recordFd);
    idInfoLen = nBytes / sizeof(Order);
    idInfo = (Order*)malloc(sizeof(Order) * idInfoLen);
    if (safeRead(recordFd, (char*)idInfo, nBytes) < 0)
        ERR_EXIT("failed to read file");

    lockInfo = (LockInfo*)malloc(sizeof(LockInfo) * idInfoLen);
    memset(lockInfo, 0,
           sizeof(LockInfo) * idInfoLen);  // UNLCK and NULL is zero
    // release read lock on whole file
    acquireLockBlocking(recordFd, F_UNLCK, SEEK_SET, 0, 0);
    // close recordFd when process exits
    return 0;
}

static int getIndexOfId(int id) {
    for (int i = 0; i < idInfoLen; ++i) {
        if (abs(idInfo[i].id) == id) {
            return i;
        }
    }
    return -1;
}

static ssize_t safeRead(int fd, char* buf, size_t count) {
    char* bufPtr = buf;
    int ret;
    while (count) {
        ret = read(fd, bufPtr, count);
        if (ret >= 0) {
            count -= ret;
            bufPtr += ret;
        } else if (errno != EINTR)
            return -1;
    }
    return ret;
}

static int acquireLockBlocking(int fd, short type, short whence, off_t offset,
                               off_t len) {
    struct flock lock = {
        .l_type = type, .l_whence = whence, .l_start = offset, .l_len = len
        // omit l_pid
    };
    int ret;
    do {
        ret = fcntl(fd, F_SETLKW, &lock);
        if (ret != -1 || errno != EINTR)
            break;
    } while (1);
    return ret;
}

static int acquireLock(int fd, short type, short whence, off_t offset,
                       off_t len) {
    struct flock lock = {
        .l_type = type, .l_whence = whence, .l_start = offset, .l_len = len
        // omit l_pid
    };
    return fcntl(fd, F_SETLK, &lock);
}

static int tryAcquireReadLock(Request* req, int idx) {
    if (lockInfo[idx].status == WRLCK) {
        return -1;
    }
    int ret = acquireLock(recordFd, F_RDLCK, SEEK_SET, idx * sizeof(Order),
                          sizeof(Order));
    if (ret == 0) {
        lockInfo[idx].status = RDLCK;
        lockInfo[idx].owner = req;
#ifndef NDEBUG
        fprintf(stderr, "read lock[%d] acquired\n", idx);
#endif
    }
    return ret;
}

static int tryAcquireWriteLock(Request* req, int idx) {
    if (lockInfo[idx].status == WRLCK) {
        return -1;
    }
    int ret = acquireLock(recordFd, F_WRLCK, SEEK_SET, idx * sizeof(Order),
                          sizeof(Order));
    if (ret == 0) {
        lockInfo[idx].status = WRLCK;
        lockInfo[idx].owner = req;
#ifndef NDEBUG
        fprintf(stderr, "write lock[%d] acquired\n", idx);
#endif
    }
    return ret;
}

static int releaseLock(Request* req, int idx) {
    if (lockInfo[idx].status == UNLCK) {
        return 0;
    } else if (lockInfo[idx].owner != req) {
        return -1;
    }
    int ret = acquireLock(recordFd, F_UNLCK, SEEK_SET, idx * sizeof(Order),
                          sizeof(Order));
    if (ret == 0) {
        lockInfo[idx].status = UNLCK;
        lockInfo[idx].owner = NULL;
#ifndef NDEBUG
        fprintf(stderr, "lock[%d] released\n", idx);
#endif
    }
    return ret;
}

static off_t getFileSize(int fd) {
    static struct stat status;
    if (fstat(fd, &status) < 0) {
        ERR_EXIT("failed to get file size");
        return 0;
    }
    return status.st_size;
}