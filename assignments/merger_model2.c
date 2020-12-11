#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERR_EXIT(msg)    \
    do {                 \
        perror(msg);     \
        if (errno)       \
            exit(errno); \
        else             \
            exit(1);     \
    } while (0)

int main(int argc, char const *argv[]) {
    int i, pid;
    int pipeFd[2];
    /// Section A
    if (pipe(pipeFd) < 0)
        ERR_EXIT("pipe error");

    for (i = 1; i < argc; i++) {
        /// Section B
        pid = fork();
        if (pid == 0) {
            /// Section C
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
    }
    /// Section E
    close(pipeFd[1]);
    int n;
    char buf[1024];
    while (1) {
        n = read(pipeFd[0], buf, sizeof(buf));
        if (n < 0)
            ERR_EXIT("read error");
        else if (n == 0)  // EOF
            break;
        if (write(STDOUT_FILENO, buf, n) < 0)
            ERR_EXIT("write error");
    }
    // TODO close pipeFd[0]?
    for (i = 1; i < argc; i++) {
        wait(NULL);
    }
    return 0;
}
