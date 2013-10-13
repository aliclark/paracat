
/*
 * Copyright (c) 2013, Ali Clark <ali@clark.gb.net>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>

#include <unistd.h>

#ifndef BUF_COUNT
/* 4096 seems a good value from tesing */
#define BUF_COUNT 4096
#endif

#ifndef NUM_BASE
#define NUM_BASE 10
#endif

#define NEWLINE_CH 10

#define FALSE 0
#define TRUE 1

#define STDIN_FD 0
#define STDOUT_FD 1

#define GOOD 0
#define ERR -1

#define CHILD 0
#define NO_OPTIONS 0

#define USAGE_STRING "Usage: paracat -n NUMPROCS -- COMMAND ARG1 ARG2 ...\n"

#ifndef __cplusplus
typedef int bool;
#endif

static int write_fully(int fd, char* buf, int count) {
    do {
        int written = write(fd, buf, count);

        if (written < GOOD) {
            fprintf(stderr, "Error: Could not write from parent to fd: %d, %s\n", fd, strerror(errno));
            return written;
        }
        buf += written;
        count -= written;

    } while (count > 0);

    return GOOD;
}

static int get_nfds(int* outfds, int numchildren) {
    int i, nfds = -1;
    for (i = 0; i < numchildren; ++i) {
        int outfd = outfds[i];
        if (outfd > nfds) {
            nfds = outfd;
        }
    }
    nfds += 1;

    return nfds;
}

static int read_write_from_children(int* outfds, int numchildren) {
    int i, nfds;
    fd_set rfds;
    char** buffers = (char**)malloc(sizeof(char*) * numchildren);
    int* buffered = (int*)malloc(sizeof(int) * numchildren);

    for (i = 0; i < numchildren; ++i) {
        buffers[i] = (char*)malloc(sizeof(char) * BUF_COUNT);
        buffered[i] = 0;
    }

    nfds = get_nfds(outfds, numchildren);

    FD_ZERO(&rfds);
    for (i = 0; i < numchildren; ++i) {
        FD_SET(outfds[i], &rfds);
    }

    while (TRUE) {
        if (select(nfds, &rfds, NULL, NULL, NULL) < GOOD) {
            perror("Error: could not select child input");
        }

        for (i = 0; i < numchildren; ++i) {
            int curfd = outfds[i];

            if (FD_ISSET(curfd, &rfds)) {
                char* buf = buffers[i];
                int saved = buffered[i];

                while (TRUE) {
                    int nlpos, j;
                    char* buf_part_top;
                    int data_size = read(curfd, buf + saved, BUF_COUNT - saved);

                    if (data_size <= 0) {

                        if (saved > 0) {
                            if (write_fully(STDOUT_FD, buf, saved) < GOOD) {
                                return ERR;
                            }
                        }
                        if (data_size < GOOD) {
                            return ERR;
                        }

                        for (j = i + 1; j < numchildren; ++j) {
                            outfds[j-1] = outfds[j];
                            buffers[j-1] = buffers[j];
                            buffered[j-1] = buffered[j];
                        }
                        --numchildren;

                        if (numchildren == 0) {
                            return GOOD;
                        }

                        FD_CLR(curfd, &rfds);
                        nfds = get_nfds(outfds, numchildren);

                        /* re-evaluate this loop number */
                        --i;
                        break;
                    }

                    data_size += saved;
                    buf_part_top = buf + data_size;

                    while (buf_part_top --> buf) {
                        if (*buf_part_top == NEWLINE_CH) {
                            break;
                        }
                    }

                    nlpos = buf_part_top - buf;

                    if (nlpos < GOOD) {
                        if (write_fully(STDOUT_FD, buf, data_size) < GOOD) {
                            return ERR;
                        }

                        saved = 0;

                    } else {
                        int part_one = nlpos + 1;

                        if (write_fully(STDOUT_FD, buf, part_one) < GOOD) {
                            return ERR;
                        }

                        saved = data_size - part_one;

                        /* XXX: overlap? */
                        memcpy(buf, buf + part_one, saved);
                        buffered[i] = saved;

                        break;
                    }
                }

            } else {
                FD_SET(curfd, &rfds);
            }
        }
    }
}

static int spawn_children(pid_t* pids, int* fds, int numchildren, char** args, bool recombine_flag, pid_t* recombine_pid) {
    int fd[2];
    int out[2];
    int i;

    int* outfds = NULL;
    pid_t pid;

    if (recombine_flag) {
        outfds = (int*)malloc(sizeof(int) * numchildren);
    }

    for (i = 0; i < numchildren; ++i) {

        if (pipe(fd) < GOOD) {
            perror("Error: Could not create input communication pipe");
            return ERR;
        }

        if (recombine_flag) {
            if (pipe(out) < GOOD) {
                perror("Error: Could not create output communication pipe");
                return ERR;
            }
        }

        pid = fork();

        if (pid < GOOD) {
            perror("Error: Could not fork a child process");
            return pid;
        }

        if (pid == CHILD) {

            if (dup2(fd[0], STDIN_FD) < GOOD) {
                perror("Error: Could not duplicate child process input to stdin");
                _exit(1);
            }

            if (close(fd[1]) < GOOD) {
                perror("Error: Could not close child process input pipe's output");
                _exit(2);
            }

            if (recombine_flag) {
                if (dup2(out[1], STDOUT_FD) < GOOD) {
                    perror("Error: Could not duplicate child process output to stdout");
                    exit(1);
                }

                if (close(out[0]) < GOOD) {
                    perror("Error: Could not close child process output pipe's input");
                    exit(2);
                }
            }

            execv(args[0], args);
            _exit(3);

        } else {
            if (close(fd[0]) < GOOD) {
                perror("Error: Could not close parent process pipe's input");
                return ERR;
            }
            fds[i] = fd[1];

            if (recombine_flag) {
                if (close(out[1]) < GOOD) {
                    perror("Error: Could not close parent process pipe's input");
                    return ERR;
                }
                outfds[i] = out[0];
            }

            pids[i] = pid;
        }
    }

    if (recombine_flag) {
        pid = fork();

        if (pid < GOOD) {
            perror("Error: Could not fork a child process");
            return pid;
        }

        if (pid == CHILD) {
            for (i = 0; i < numchildren; ++i) {
                if (close(fds[i]) < GOOD) {
                    perror("Error: Could not close parent process pipe's output");
                    _exit(1);
                }
            }

            if (read_write_from_children(outfds, numchildren) < GOOD) {
                _exit(1);
            }

            _exit(0);

        } else {
            *recombine_pid = pid;

            for (i = 0; i < numchildren; ++i) {
                if (close(outfds[i]) < GOOD) {
                    perror("Error: Could not close parent process pipe's output");
                    return ERR;
                }
            }
        }
    }

    return GOOD;
}

static int read_write_loop(int* fds, int fdtop) {
    int fdpos = 0;
    int curfd = fds[fdpos];
    char buf[BUF_COUNT];

    int part_two = 0;
    int read_amount = BUF_COUNT;

    while (TRUE) {
        char* buf_part_top;
        int nlpos;
        int data_size = read(STDIN_FD, buf + part_two, read_amount);

        if (data_size <= 0) {
            if (part_two > 0) {
                if (write_fully(curfd, buf, part_two) < GOOD) {
                    return ERR;
                }
            }
            if (data_size < GOOD) {
                perror("Error: Could not read from parent stdin");
            }
            return data_size;
        }

        data_size += part_two;
        buf_part_top = buf + data_size;

        while (buf_part_top --> buf) {
            if (*buf_part_top == NEWLINE_CH) {
                break;
            }
        }

        nlpos = buf_part_top - buf;

        if (nlpos < GOOD) {
            if (write_fully(curfd, buf, data_size) < GOOD) {
                return ERR;
            }

            part_two = 0;
            read_amount = BUF_COUNT;

        } else {
            int part_one = nlpos + 1;

            if (write_fully(curfd, buf, part_one) < GOOD) {
                return ERR;
            }

            if (fdpos >= fdtop) {
                fdpos = 0;
            } else {
                ++fdpos;
            }
            curfd = fds[fdpos];

            part_two = data_size - part_one;
            read_amount = BUF_COUNT - part_two;

            /* XXX: overlap? */
            memcpy(buf, buf + part_one, part_two);
        }
    }
}

int main(int argc, char** argv) {
    pid_t* pids;
    int* fds;
    int i;
    int numpids = 0;
    char* end = NULL;
    char** command;
    int status;

    pid_t recombine_pid = 0;
    int recombine_flag = 1;

    struct option long_options[] = {
        {"no-recombine", no_argument, &recombine_flag, 0},
        {"help",         no_argument, NULL,            'h'},
        {0, 0, 0, 0}
    };

    char** argv_copy = (char**)malloc(sizeof(char**) * argc);
    for (i = 0; i < argc; ++i) {
        argv_copy[i] = argv[i];
    }

    while (TRUE) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "hn:", long_options, &option_index);
     
        if (c == -1) {
            break;
        }

        switch (c) {
        case 0:
            break;

        case 'h':
            fputs(USAGE_STRING, stderr);
            return 0;
     
        case 'n':
            numpids = strtol(optarg, &end, NUM_BASE);
            if (*end) {
                fprintf(stderr, "Error: Could not parse spawn count: %s\n", end);
                fputs(USAGE_STRING, stderr);
                return 1;
            }
            if (numpids < 1) {
                fputs("Error: spawn count must be 1 or greater\n", stderr);
                fputs(USAGE_STRING, stderr);
                return 1;
            }
            break;
     
        default:
            fputs(USAGE_STRING, stderr);
            return 1;
        }
    }

    if (numpids == 0) {
        fputs("Parameter -n is required.\n", stderr);
        fputs(USAGE_STRING, stderr);
        return 5;
    }

    if (strcmp("--", argv[optind - 1]) != GOOD) {
        fputs("Command separator -- is required.\n", stderr);
        fputs(USAGE_STRING, stderr);
        return 5;
    }

    for (i = 0; i < argc; ++i) {
        if (strcmp("--", argv_copy[i]) == 0) {
            if (i != (optind - 1)) {
                fputs("Unrecognised arguments before -- separator.\n", stderr);
                fputs(USAGE_STRING, stderr);
                return 5;
            }
        }
    }

    if (optind >= argc) {
        fputs("Please specify a command after the -- separator.\n", stderr);
        fputs(USAGE_STRING, stderr);
        return 5;
    }

    command = argv + optind;

    pids = (pid_t*)malloc(sizeof(pid_t) * numpids);
    fds = (int*)malloc(sizeof(int) * numpids);

    if (spawn_children(pids, fds, numpids, command, recombine_flag, &recombine_pid) < GOOD) {
        return 2;
    }

    if (read_write_loop(fds, numpids - 1) < GOOD) {
        return 3;
    }

    for (i = 0; i < numpids; ++i) {
        if (close(fds[i]) < GOOD) {
            fprintf(stderr, "Error: Could not close child stdin fd: %d, %s\n", fds[i], strerror(errno));
            /* continue anyway */
        }
    }

    for (i = 0; i < numpids; ++i) {
        if (waitpid(pids[i], &status, NO_OPTIONS) < GOOD) {
            fprintf(stderr, "Error: Could not wait for child pid: %d, %s\n", pids[i], strerror(errno));
            /* continue anyway */
        }

        if (status != GOOD) {
            fprintf(stderr, "Warning: got exit status: %d, from child pid: %d\n", status, pids[i]);
        }
    }

    if (recombine_flag) {
        if (waitpid(recombine_pid, &status, NO_OPTIONS) < GOOD) {
            fprintf(stderr, "Error: Could not wait for reader pid: %d, %s\n", pids[i], strerror(errno));
            /* continue anyway */
        }

        if (status != GOOD) {
            fprintf(stderr, "Warning: got exit status: %d, from reader pid: %d\n", status, pids[i]);
        }
    }

    return EXIT_SUCCESS;
}
