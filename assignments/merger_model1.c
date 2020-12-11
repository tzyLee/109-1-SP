#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

int errnoCopy;
#define ERR_EXIT(msg)        \
    do {                     \
        errnoCopy = errno;   \
        perror(msg);         \
        if (errnoCopy)       \
            exit(errnoCopy); \
        else                 \
            exit(1);         \
    } while (0)

int main(int argc, char const *argv[]) {
    int i, pid;
    int pipeFd[2];
    /// Section A
    int *readFds = (int *)malloc((argc - 1) * sizeof(int));
    if (readFds == NULL) {
        ERR_EXIT("malloc error");
    }
    for (i = 1; i < argc; i++) {
        /// Section B
        if (pipe(pipeFd) < 0)
            ERR_EXIT("pipe error");
        pid = fork();
        if (pid == 0) {
            /// Section C
            for (int j = 1; j < i; j++)
                close(readFds[j]);

            if (pipeFd[1] != STDOUT_FILENO) {
                if (dup2(pipeFd[1], STDOUT_FILENO) != STDOUT_FILENO)
                    ERR_EXIT("dup2 error");
                close(pipeFd[1]);
            }
            close(pipeFd[0]);
            if (execlp(argv[i], argv[i], (char *)0) < 0)
                ERR_EXIT("execlp error");
        }
        /// Section D
        else if (pid < 0) {
            ERR_EXIT("fork error");
        }
        close(pipeFd[1]);
        readFds[i - 1] = pipeFd[0];
    }
    /// Section E
    int rCount = argc - 1;
    int maxRFd = 0;
    fd_set rfds, workingRFds;
    FD_ZERO(&rfds);
    for (int i = 0; i < rCount; ++i) {
        FD_SET(readFds[i], &rfds);
        if (readFds[i] > maxRFd)
            maxRFd = readFds[i];
    }
    ++maxRFd;
    int n;
    char buf[1024];
    while (rCount > 0) {  // There's remaining fd to read
        memcpy(&workingRFds, &rfds, sizeof(rfds));
        if (select(maxRFd, &workingRFds, NULL, NULL, NULL) < 0)
            ERR_EXIT("select error");

        for (int i = 0; i < rCount; ++i) {
            if (FD_ISSET(readFds[i], &workingRFds)) {
                n = read(readFds[i], buf, sizeof(buf));
                if (n < 0)
                    ERR_EXIT("read error");
                else if (n == 0) {  // EOF
                    FD_CLR(readFds[i], &rfds);
                    rCount -= 1;
                    // `rCount-1` is always in range [0, argc-1], no range check
                    // is needed
                    int temp = readFds[i];
                    readFds[i] = readFds[rCount];
                    readFds[rCount] = temp;
                    // Use swap to keep all fds in the array (for closing later)
                    continue;
                }
                if (write(STDOUT_FILENO, buf, n) < 0)
                    ERR_EXIT("write error");
            }
        }
    }
    // close readFds
    for (int i = 1; i < argc; ++i)
        close(readFds[i - 1]);
    free(readFds);
    for (i = 1; i < argc; i++)
        wait(NULL);
    return 0;
}
