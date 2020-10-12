
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

    // Initialize server
    initServer((unsigned short)atoi(argv[1]));

    // Loop for handling connections
    fprintf(stderr, "\nstarting on %.80s, port %d, fd %d, maxconn %d...\n",
            svr.hostname, svr.port, svr.listen_fd, maxFd);

    while (1) {
#ifndef NDEBUG
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
                writeStr(fd, "Operation failed.");
                cleanUpRequest(&requests[fd]);
                break;
            case LOCKED:
                writeStr(fd, "Locked.");
                cleanUpRequest(&requests[fd]);
                break;
            }
            if (--totalFd == 0)
                break;
        }
    }
    free(requests);
    free(pollFds);
    requests = NULL;
    pollFds = NULL;
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

static void removeFd(int idx) {
    // assume 0 <= pollFdsLen < maxFds;
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
    int nRead = read(req->conn_fd, buf, sizeof(buf));
    if (nRead < 512) {
        buf[nRead] = '\0';
    }
    strncpy(req->buf, buf, 512);
#ifndef NDEBUG
    strtok(buf, "\r");
    strtok(buf, "\n");
    fprintf(stderr, "received '%s' from fd %d\n", buf, req->conn_fd);
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
    readRequest(req);
    sprintf(buf, "You can order %d adult mask(s) and %d children mask(s).\n", 0,
            0);
    write(req->conn_fd, buf, strlen(buf));
    // TODO
    return SUCCESS;
}

static Result handleOrder(Request* req) {
    readRequest(req);
    int quantity = 0;
    sprintf(buf, "Pre-order for %d succeeded, %d %s mask(s) ordered.\n",
            req->id, quantity, "adult");
    write(req->conn_fd, buf, strlen(buf));
    // TODO
    return SUCCESS;
}
