#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

/*
    ./a.out < infile 2>&1 > outfile
    0: STDIN  -> infile
    1: STDOUT -> outfile
    2: STDERR -> original STDOUT
*/

int main(int argc, char* argv[]) {
    int fd1, fd2;
    assert(argc > 2);
    char *infile = argv[1], *outfile = argv[2];
    fd1 = open(infile, O_RDONLY);
    fd2 = open(outfile, O_WRONLY | O_CREAT, 0666);

    dup2(fd1, STDIN_FILENO);
    close(fd1);

    dup2(STDOUT_FILENO, STDERR_FILENO);

    dup2(fd2, STDOUT_FILENO);
    close(fd2);

    execlp("./a.out", "./a.out", (char*)0);
    return 0;
}
