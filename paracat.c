
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

/* requires -std=gnu89 at a minimum (or default) for PIPE_BUF constant */
#include <limits.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>

#include <unistd.h>

#ifndef PIPE_BUF
/* POSIX.1-2001 requies at least 512, though would be ideal to have 4096 as in Linux */
#define PIPE_BUF 512
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

static int string_char_count(char* str, char c) {
    int count = 0;

    while (TRUE) {
        char x = *str++;
        if (x == '\0') {
            break;
        }
        if (x == c) {
            ++count;
        }
    }
    return count;
}

static char* sh_escape_string(char* dest, char* src) {
    *dest++ = '\'';
    while (TRUE) {
        char x = *src++;
        if (x == '\'') {
            *dest++ = '\'';
            *dest++ = '\\';
            *dest++ = '\'';
        } else if (x == '\0') {
            *dest++ = '\'';
            break;
        }
        *dest++ = x;
    }
    *dest = '\0';
    return dest;
}

/*
 * Return a string that can be passed to /bin/sh -c
 */
static char* sh_build_command(char** args) {
    char* dest;
    char* dest_start;
    int i, count = 0;

    if (!args[0]) {
        return NULL;
    }

    for (i = 0; args[i]; ++i) {
        int quote_count = string_char_count(args[i], '\'');
        /* on first iteration the +1 is for the final '\0'
         * on the others it's for the space separator
         */
        count += strlen(args[i]) + (quote_count * 4) + 2 + 1;
    }

    dest = (char*)malloc(sizeof(char) * count);
    dest_start = dest;

    for (i = 0; args[i]; ++i) {
        dest = sh_escape_string(dest, args[i]);
        *dest++ = ' ';
    }
    *(dest - 1) = '\0';

    return dest_start;
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

static void log_write_err(int fd) {
    fprintf(stderr, "Error: Could not write from parent to fd: %d, %s\n", fd, strerror(errno));
}

static int write_fully(int fd, char* buf, int count) {
    do {
        int written = write(fd, buf, count);

        if (written < GOOD) {
            log_write_err(fd);
            return written;
        }
        buf += written;
        count -= written;

    } while (count > 0);

    return GOOD;
}

static int read_write_from_children(int* outfds, int numchildren) {
    int i, nfds;
    fd_set rfds;
    char** buffers = (char**)malloc(sizeof(char*) * numchildren);
    int* buffered = (int*)malloc(sizeof(int) * numchildren);

    for (i = 0; i < numchildren; ++i) {
        buffers[i] = (char*)malloc(sizeof(char) * PIPE_BUF);
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
                    int data_size = read(curfd, buf + saved, PIPE_BUF - saved);

                    if (data_size <= 0) {

                        if (saved > 0) {
                            if (write_fully(STDOUT_FD, buf, saved) < GOOD) {
                                return ERR;
                            }
                        }
                        if (data_size < GOOD) {
                            perror("Error: Could not read from child stdout");
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

static int read_write_loop(int* fds, int fdtop) {
    int fdpos = 0;
    int curfd = fds[fdpos];
    char buf[PIPE_BUF];

    int saved = 0;
    int read_amount = PIPE_BUF;

    while (TRUE) {
        char* buf_part_top;
        int nlpos;
        int data_size = read(STDIN_FD, buf + saved, read_amount);

        if (data_size <= 0) {
            if (saved > 0) {
                if (write_fully(curfd, buf, saved) < GOOD) {
                    return ERR;
                }
            }
            if (data_size < GOOD) {
                perror("Error: Could not read from parent stdin");
            }
            return data_size;
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
            if (write_fully(curfd, buf, data_size) < GOOD) {
                return ERR;
            }

            saved = 0;
            read_amount = PIPE_BUF;

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

            saved = data_size - part_one;
            read_amount = PIPE_BUF - saved;

            /* XXX: overlap? */
            memcpy(buf, buf + part_one, saved);
        }
    }
}

static int spawn_children(pid_t* pids, int* fds, int numchildren, char** args, bool recombine_flag, pid_t* recombine_pid, bool use_sh) {
    char* sh_start[] = { (char*)"/bin/sh", (char*)"-c", NULL, NULL };
    int fd[2];
    int out[2];
    int i;

    int* outfds = NULL;
    pid_t pid;

    if (recombine_flag) {
        outfds = (int*)malloc(sizeof(int) * numchildren);
    }

    if (use_sh) {
        sh_start[2] = sh_build_command(args);
        args = sh_start;
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

int main(int argc, char** argv) {
    pid_t* pids;
    int* fds;
    int i, childfail;
    int numpids = 0;
    char* end = NULL;
    char** command;
    char** argv_copy;
    char* default_command[] = { "/bin/cat", NULL };
    int status;

    pid_t recombine_pid = 0;
    int recombine_flag = 1;
    int use_sh_flag = 1;

    struct option long_options[] = {
        {"no-recombine", no_argument, NULL, 0},
        {"no-shell",     no_argument, NULL, 0},
        {"help",         no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };
    long_options[0].flag = &recombine_flag;
    long_options[1].flag = &use_sh_flag;

    argv_copy = (char**)malloc(sizeof(char**) * argc);
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
        /* Default to 2 if not specified */
        numpids = 2;
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
            break;
        }
    }

    if (optind >= argc) {
        command = default_command;
    } else {
        command = argv + optind;
    }

    pids = (pid_t*)malloc(sizeof(pid_t) * numpids);
    fds = (int*)malloc(sizeof(int) * numpids);

    if (spawn_children(pids, fds, numpids, command, recombine_flag, &recombine_pid, use_sh_flag) < GOOD) {
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

    childfail = 0;
    for (i = 0; i < numpids; ++i) {
        if (waitpid(pids[i], &status, NO_OPTIONS) < GOOD) {
            fprintf(stderr, "Error: Could not wait for child pid: %d, %s\n", pids[i], strerror(errno));
            /* continue anyway */
        }

        if (status != GOOD) {
            fprintf(stderr, "Warning: got exit status: %d, from child pid: %d\n", status, pids[i]);
            childfail |= 8;
        }
    }

    if (recombine_flag) {
        if (waitpid(recombine_pid, &status, NO_OPTIONS) < GOOD) {
            fprintf(stderr, "Error: Could not wait for reader pid: %d, %s\n", recombine_pid, strerror(errno));
            /* continue anyway */
        }

        if (status != GOOD) {
            fprintf(stderr, "Warning: got exit status: %d, from reader pid: %d\n", status, recombine_pid);
            childfail |= 16;
        }
    }

    return childfail;
}
