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

typedef struct {
    char hostname[512];   // server's hostname
    unsigned short port;  // port to listen
    int listen_fd;        // fd to wait for a new connection
} server;

typedef struct {
    char host[512];  // client's host
    int conn_fd;     // fd to talk with client
    char buf[512];   // data sent by/to client
    size_t buf_len;  // bytes used by buf
    // you don't need to change this.
    int id;
    int wait_for_write;  // used by handle_read to know if the header is read or
                         // not.
} request;

server svr;                // server
request* requestP = NULL;  // point to a list of requests
int maxfd;  // size of open file descriptor table, size of request list
struct pollfd* fds = NULL;  // point to a list of pollfds
int fds_len = 0;            // number of pollfds used in fds

const char* accept_read_header = "ACCEPT_FROM_READ";
const char* accept_write_header = "ACCEPT_FROM_WRITE";
const char* id_prompt =
    "Please enter the id (to check how many masks can be ordered):";
const char* order_prompt =
    "Please enter the mask type (adult or children) and number of mask you "
    "would like to order:";

static void init_server(unsigned short port);
// initailize a server, exit for error

static void init_request(request* reqP);
// initailize a request instance

static void free_request(request* reqP);
// free resources used by a request instance

static void block_and_accept_connection(int sockfd,
                                        struct sockaddr_in* cliaddrp,
                                        int* clilen);

static int find_id_in_file(int id, const Order* order);

static void append_fd(int fd);
// append a fd to the end of fds

static void remove_fd(int idx);
// remove fds[idx]

typedef struct {
    int id;  // customer id
    int adultMask;
    int childrenMask;
} Order;

int handle_read(request* reqP) {
    char buf[512];
    int nRead = read(reqP->conn_fd, buf, sizeof(buf));
    if (nRead < 512) {
        buf[nRead] = '\0';
    }
    strncpy(reqP->buf, buf, 512);
    return nRead;
}

int main(int argc, char** argv) {
    // Parse args.
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        exit(1);
    }

    struct sockaddr_in cliaddr;  // used by accept()
    Order order;
    int clilen = sizeof(cliaddr);

    int conn_fd;  // fd for a new connection with client
    int file_fd;  // fd for file that we open for reading
    char buf[512];
    int buf_len;
    int total_fd;  // number of fds that are ready for reading

    // Initialize server
    init_server((unsigned short)atoi(argv[1]));

    fds = (struct pollfd*)malloc(sizeof(struct pollfd) * maxfd);
    fds_len = 0;

    // Loop for handling connections
    fprintf(stderr, "\nstarting on %.80s, port %d, fd %d, maxconn %d...\n",
            svr.hostname, svr.port, svr.listen_fd, maxfd);

    append_fd(svr.listen_fd);

    while (1) {
        // TODO: Add IO multiplexing
        if (fds_len == 1) {
            // No pending requests, check for new connection
            block_and_accept_connection(svr.listen_fd, &cliaddr, &clilen);
        } else {
            // Has Pending requests
            if ((total_fd = poll(fds, fds_len, -1)) < 0) {
                ERR_EXIT("poll");
            }
            if (total_fd == 0)
                continue;
            // Check for incoming connection
            // prevents the server from blocked by waiting for sockets to answer
            if (fds[0].revents != 0) {
                block_and_accept_connection(svr.listen_fd, &cliaddr, &clilen);
                fds[0].revents = 0;
                if (--total_fd == 0)
                    continue;
            }
            // TODO: handle requests from clients
            for (int i = 1; i < fds_len;) {
                if (fds[i].revents == 0) {
                    ++i;
                    continue;
                }
                int fd = fds[i].fd;
                int ret =
                    handle_read(&requestP[fd]);  // parse data from client to
                                                 // requestP[conn_fd].buf
                if (ret < 0) {
                    fprintf(stderr, "bad request from %s\n", requestP[fd].host);
                    continue;
                }
                fprintf(stderr, "handling request %d, %d can be read\n", i,
                        total_fd);
                if (ret != 0) {
                    int id = atoi(requestP[fd].buf);
                    if (find_id_in_file(id, &order) == -1) {
                        write(requestP[fd].conn_fd, "Operation failed.",
                              strlen("Operation failed."));
                        close(requestP[fd].conn_fd);
                        free_request(&requestP[fd]);
                        remove_fd(i);
                        if (--total_fd == 0)
                            break;
                        continue;
                    } else {
                        sprintf(buf,
                                "You can order %d adult mask(s) and %d "
                                "children mask(s).",
                                order.adultMask, order.childrenMask);
                        write(requestP[fd].conn_fd, buf, strlen(buf));
#ifdef READ_SERVER
#else
#endif
                    }
#ifdef READ_SERVER
                    fprintf(stderr, "%s", requestP[fd].buf);
                    sprintf(buf, "%s : %s", accept_read_header,
                            requestP[fd].buf);
                    write(requestP[fd].conn_fd, buf, strlen(buf));
#else
                    fprintf(stderr, "%s", requestP[fd].buf);
                    sprintf(buf, "%s : %s", accept_write_header,
                            requestP[fd].buf);
                    write(requestP[fd].conn_fd, buf, strlen(buf));
#endif
                }
                close(requestP[fd].conn_fd);
                free_request(&requestP[fd]);
                remove_fd(i);
                if (--total_fd == 0)
                    break;
            }
        }
    }
    free(requestP);
    free(fds);
    fds = NULL;  // clear dangling pointer
    return 0;
}

// ======================================================================================================
static void block_and_accept_connection(int sockfd,
                                        struct sockaddr_in* cliaddrp,
                                        int* clilen) {
    int conn_fd = 0;
    do {
        conn_fd =
            accept(sockfd, (struct sockaddr*)cliaddrp, (socklen_t*)clilen);
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
                          maxfd);
            return;  // Stop accept new connection to allow the requests to be
                     // done
        default:
            ERR_EXIT("accept");
        }
    } while (1);
    requestP[conn_fd].conn_fd = conn_fd;
    strcpy(requestP[conn_fd].host, inet_ntoa(cliaddrp->sin_addr));
    append_fd(conn_fd);
    fprintf(stderr, "getting a new request... fd %d from %s\n", conn_fd,
            requestP[conn_fd].host);
    // prompt for id
    write(conn_fd, id_prompt, strlen(id_prompt));
}

static int find_id_in_file(int id, const Order* order) {
    static struct flock lock = {
        .l_type = F_RDLCK, .l_start = 0, .l_whence = SEEK_SET, .l_len = 0};
    int fd = open("./preorderRecord", O_RDONLY);
    while (fcntl(fd, F_SETLKW, &lock) == -1 && errno == EACCES) {
        // acquire read lock
    }
    int res = 0;
    while ((res = read(fd, order, sizeof(Order))) > 0) {
        if (order->id == id) {
            break;
        }
    }
    close(fd);
    return -1;  // id not found in file
}

static void append_fd(int fd) {
    // assume 0 <= fds_len < maxfds;
    fds[fds_len].fd = fd;
    fds[fds_len].events = POLLIN;  // can be read or hung up
    fds[fds_len].revents = 0;
    ++fds_len;
}

static void remove_fd(int idx) {
    // assume 0 <= fds_len < maxfds;
    if (idx != fds_len - 1) {
        memcpy(fds + idx, fds + fds_len - 1, sizeof(struct pollfd));
    }
    --fds_len;
}

// ======================================================================================================
// You don't need to know how the following codes are working
#include <fcntl.h>

static void init_request(request* reqP) {
    reqP->conn_fd = -1;
    reqP->buf_len = 0;
    reqP->id = 0;
}

static void free_request(request* reqP) {
    /*if (reqP->filename != NULL) {
        free(reqP->filename);
        reqP->filename = NULL;
    }*/
    init_request(reqP);
}

static void init_server(unsigned short port) {
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
    maxfd = getdtablesize();
    requestP = (request*)malloc(sizeof(request) * maxfd);
    if (requestP == NULL) {
        ERR_EXIT("out of memory allocating all requests");
    }
    for (int i = 0; i < maxfd; i++) {
        init_request(&requestP[i]);
    }
    requestP[svr.listen_fd].conn_fd = svr.listen_fd;
    strcpy(requestP[svr.listen_fd].host, svr.hostname);

    return;
}
