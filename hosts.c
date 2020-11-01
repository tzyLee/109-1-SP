#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int retCode = 0;
#define ERR_EXIT(s)      \
    do {                 \
        retCode = errno; \
        perror(s);       \
        exit(retCode);   \
    } while (0)

/// @brief Fork a subprocess and run the given program with `argv`, and use pipe
/// to communicate between child and parent
/// @param pathname path to the program for child to run
/// @param argv     argv sent to the program
/// @param pid      the pid of child
/// @param inFile   file read from stdout of child
/// @param outFile  file write to stdin of child
void run(const char* pathname, char* const argv[], int* pid, FILE** inFile,
         FILE** outFile) {
    int fd[2][2] = {0};
    // fd[0]: parent(fd[0][0]) <- child(fd[0][1])
    // fd[1]: parent(fd[1][1]) -> child(fd[1][0])
    if ((inFile != NULL && pipe(fd[0]) < 0) ||
        (outFile != NULL && pipe(fd[1]) < 0)) {
        ERR_EXIT("Error opening pipe");
    }

    if ((*pid = fork()) < 0) {
        ERR_EXIT("Error forking");
    } else if (*pid == 0) {
        // Child
        if (inFile != NULL) {
            close(fd[0][0]);
            if (fd[0][1] != STDOUT_FILENO) {
                if (dup2(fd[0][1], STDOUT_FILENO) != STDOUT_FILENO) {
                    ERR_EXIT("Redirection failed");
                }
                close(fd[0][1]);
            }
        }
        if (outFile != NULL) {
            close(fd[1][1]);
            if (fd[1][0] != STDIN_FILENO) {
                if (dup2(fd[1][0], STDIN_FILENO) != STDIN_FILENO) {
                    ERR_EXIT("Redirection failed");
                }
                close(fd[1][0]);
            }
        }
        if (execv(pathname, argv) < 0) {
            ERR_EXIT("Execv error");
        }
        _exit(0);
    } else {
        // Parent
        if (inFile != NULL) {
            close(fd[0][1]);
            *inFile = fdopen(fd[0][0], "r");
        }
        if (outFile != NULL) {
            close(fd[1][0]);
            *outFile = fdopen(fd[1][1], "w");
        }
    }
}

void scoreToRank(int n, const int* score, int* rank) {
    for (int i = 0; i < n; ++i) {
        int nBigger = 0;
        for (int j = 0; j < n; ++j) {
            if (score[i] < score[j])
                ++nBigger;
        }
        rank[i] = nBigger + 1;
    }
}

int main(int argc, char* const argv[]) {
    assert(argc >= 4);
    int host_id = atoi(argv[1]);
    int key = atoi(argv[2]);
    int depth = atoi(argv[3]);

    int pid[2] = {-1, -1};
    FILE* files[2][2] = {NULL};
    char num_buf[16] = {0};
    char buf[64] = {0};
    FILE* fifo[2] = {NULL};

    // Build host tree
    if (depth < 2) {
        char* const child_argv[] = {"./host", argv[1], argv[2], num_buf, NULL};
        snprintf(num_buf, sizeof(num_buf), "%d", depth + 1);
        // fork 2 hosts
        run("./host", child_argv, &pid[0], &files[0][0], &files[0][1]);
        run("./host", child_argv, &pid[1], &files[1][0], &files[1][1]);
    }

    if (depth == 0) {
        snprintf(buf, sizeof(buf), "./fifo_%d.tmp", host_id);
        fifo[0] = fopen(buf, "r");
        fifo[1] = fopen("./fifo_0.tmp", "w");
    }
    int player_id[8] = {0};
    while (1) {
        // Get player id
        int nToRead = 8 >> depth;
        player_id[0] = -1;  // fails when scanf failed
        if (depth == 0) {
            for (int i = 0; i < nToRead; ++i) {
                fscanf(fifo[0], "%d", &player_id[i]);
            }
        } else {
            for (int i = 0; i < nToRead; ++i) {
                scanf("%d", &player_id[i]);
            }
        }

        if (depth == 2) {
            if (player_id[0] == -1) {
                break;
            }
            // Start player
            char* const child_argv[] = {"./player", num_buf, NULL};
            // fork 2 players
            snprintf(num_buf, sizeof(num_buf), "%d", player_id[0]);
            run("./player", child_argv, &pid[0], &files[0][0], NULL);
            snprintf(num_buf, sizeof(num_buf), "%d", player_id[1]);
            run("./player", child_argv, &pid[1], &files[1][0], NULL);
        } else {
            // Pass player_id to child
            int half = nToRead / 2;
            fprintf(files[0][1], "%d", player_id[0]);
            for (int i = 1; i < half; ++i) {
                fprintf(files[0][1], " %d", player_id[i]);
            }
            fprintf(files[0][1], "\n");
            fflush(files[0][1]);

            fprintf(files[1][1], "%d", player_id[half]);
            for (int i = 1; i < half; ++i) {
                fprintf(files[1][1], " %d", player_id[half + i]);
            }
            fprintf(files[1][1], "\n");
            fflush(files[1][1]);
            if (player_id[0] == -1) {
                break;
            }
        }

        // Read from children, compare bids, then output
        int playerA, playerB, bidA, bidB;
        if (depth > 0) {
            for (int round = 1; round < 11; ++round) {
                fscanf(files[0][0], "%d %d", &playerA, &bidA);
                fscanf(files[1][0], "%d %d", &playerB, &bidB);
                if (bidA > bidB) {
                    printf("%d %d\n", playerA, bidA);
                } else {
                    printf("%d %d\n", playerB, bidB);
                }
                fflush(stdout);
            }
        } else {
            int score[8] = {0};
            int rank[8] = {0};
            for (int round = 1; round < 11; ++round) {
                fscanf(files[0][0], "%d %d", &playerA, &bidA);
                fscanf(files[1][0], "%d %d", &playerB, &bidB);
                if (bidA > bidB) {
                    for (int i = 0; i < 8; ++i) {
                        if (player_id[i] == playerA) {
                            ++score[i];
                            break;
                        }
                    }
                } else {
                    for (int i = 0; i < 8; ++i) {
                        if (player_id[i] == playerB) {
                            ++score[i];
                            break;
                        }
                    }
                }
            }
            scoreToRank(8, score, rank);
            // TODO mutex?
            char* bufStart = buf;
            size_t sizeRem = sizeof(buf);
            int nPrinted = 0;
            nPrinted = snprintf(bufStart, sizeRem, "%d\n", key);
            bufStart += nPrinted;
            sizeRem -= nPrinted;
            for (int i = 0; i < 8; ++i) {
                nPrinted = snprintf(bufStart, sizeRem, "%d %d\n", player_id[i],
                                    rank[i]);
                bufStart += nPrinted;
                sizeRem -= nPrinted;
            }
            if (fputs(buf, fifo[1]) == EOF) {
                ERR_EXIT("EOF when write to fifo");
            }
            fflush(fifo[1]);
        }

        // Leave host wait for player to terminate
        if (depth == 2) {
            for (int i = 0; i < 2; ++i) {
                fclose(files[i][0]);
                files[i][0] = NULL;
                if (waitpid(pid[i], NULL, 0) < 0) {
                    ERR_EXIT("Waitpid error");
                }
                pid[i] = -1;
            }
        }
    }

    // Wait for child process to finish and close file
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j)
            if (files[i][j] != NULL) {
                fclose(files[i][j]);
                files[i][j] = NULL;
            }
        if (pid[i] != -1 && waitpid(pid[i], NULL, 0) < 0) {
            ERR_EXIT("Waitpid error");
        }
        pid[i] = -1;
    }

    if (depth == 0) {
        fclose(fifo[0]);
        fclose(fifo[1]);
    }
    return 0;
}