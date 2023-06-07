#include <assert.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

enum { CMD_SIZE = 100 };

// package type
enum {
    DATA_PACKAGE = 0,
    OPEN_PACKAGE = 1,
    CLOSE_PACKAGE = 2,
};

int create_listener(char *service) {
    struct addrinfo *res = NULL;
    int gai_err;
    struct addrinfo hint = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_PASSIVE,
    };
    if ((gai_err = getaddrinfo(NULL, service, &hint, &res))) {
        fprintf(stderr, "gai error: %s\n", gai_strerror(gai_err));
        return -1;
    }
    int sock = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, 0);

        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        if (sock < 0) {
            perror("socket");
            continue;
        }
        if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
            perror("bind");
            close(sock);
            sock = -1;
            continue;
        }
        if (listen(sock, SOMAXCONN) < 0) {
            perror("listen");
            close(sock);
            sock = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    return sock;
}

struct open_package {
    size_t args_count;
    char *cmd;
    char **args;
};

struct package {
    int package_type;
    size_t size;
    char *data;
};

// OPEN_PACKAGE structure
// args_count  len(cmd)  cmd    len(args[0])  args[0]  ...
// len(args[args_count-1])  args[args_count-1] size_t      size_t    chars
// size_t        chars    ... size_t                   chars
int read_open_package(int socket, struct open_package *package) {
    ssize_t r;

    size_t args_count;
    r = recv(socket, &args_count, sizeof(args_count), MSG_WAITALL);
    assert(r == sizeof(args_count));

    size_t len_cmd;
    r = recv(socket, &len_cmd, sizeof(len_cmd), MSG_WAITALL);
    assert(r == sizeof(len_cmd));

    char *cmd = calloc(len_cmd + 1, sizeof(*cmd));
    r = recv(socket, cmd, len_cmd, MSG_WAITALL);
    assert(r == len_cmd);
    cmd[len_cmd] = '\0';

    char **args = calloc(args_count + 2, sizeof(*args));
    args[0] = cmd;
    args[args_count + 1] = NULL;
    for (size_t arg_i = 1; arg_i <= args_count; ++arg_i) {
        size_t len_arg;
        r = recv(socket, &len_arg, sizeof(len_arg), MSG_WAITALL);
        assert(r == sizeof(len_arg));

        args[arg_i] = calloc(len_arg + 1, sizeof(*args[arg_i]));
        r = recv(socket, args[arg_i], len_arg, MSG_WAITALL);
        assert(r == len_arg);
    }

    package->args_count = args_count;
    package->cmd = cmd;
    package->args = args;

    return 0;
}

volatile int child_finished_flag = 0;

void child_is_done(int sig) {
    wait(NULL);
    child_finished_flag = 1;
}

// PACKAGE structure
// package_type  size    data
// int           size_t  chars
int read_package(int socket, struct package *package) {
    ssize_t r;

    int package_type;
    r = recv(socket, &package_type, sizeof(package_type), MSG_WAITALL);
    if (r == -1) {
        return 2;
    }

    assert(r == sizeof(package_type));

    size_t size;
    r = recv(socket, &size, sizeof(size), MSG_WAITALL);
    assert(r == sizeof(size));

    char *data = calloc(size, sizeof(*data));

    for (size_t i = 0; i < size; ++i) {
        r = recv(socket, data + i, sizeof(*data), MSG_WAITALL);
        assert(r == sizeof(*data));
    }

    package->package_type = package_type;
    package->size = size;
    package->data = data;

    if (package->package_type == CLOSE_PACKAGE) {
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s SERVICE\n", argv[0]);
        return 1;
    }

    // make a daemon

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "fork error\n");
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        fprintf(stderr, "setsid error\n");
        exit(EXIT_FAILURE);
    }

    int sock = create_listener(argv[1]);
    if (sock < 0) {
        fprintf(stderr, "create_listener error\n");
        exit(EXIT_FAILURE);
    }

    while (1) {
        int connection = accept(sock, NULL, NULL);

        pid_t cmd_executor = fork();

        if (cmd_executor < 0) {
            fprintf(stderr, "executor error\n");
            exit(EXIT_FAILURE);
        }

        if (cmd_executor == 0) {
            // read open package
            struct open_package *op;
            op = malloc(sizeof(*op));
            if (read_open_package(connection, op)) {
                fprintf(stderr, "read open package error\n");
                exit(EXIT_FAILURE);
            }

            int fd_write_handler[2];

            int p = pipe(fd_write_handler);
            if (p == -1) {
                fprintf(stderr, "pipe error\n");
                exit(EXIT_FAILURE);
            }

            //  executor    fork---->  executor ----------\                 //
            //                \          ||                \                //
            //                 \         ||  pipe           \               //
            //                  \        ||                  >              //
            //                   --->  package_handler  <---  client

            pid_t executor_process = fork();

            if (executor_process < 0) {
                fprintf(stderr, "fork package handler error\n");
                exit(EXIT_FAILURE);
            }

            if (executor_process > 0) {
                // package_handler

                struct sigaction sa_sigchld = {.sa_handler = child_is_done};
                sigaction(SIGCHLD, &sa_sigchld, NULL);

                // reading packages until "STOP package"
                // sending them to executor

                struct package *p;
                p = malloc(sizeof(*p));
                while (!child_finished_flag && !read_package(connection, p)) {
                    ssize_t r = write(fd_write_handler[1], p->data, p->size);
                    if (r != p->size) {
                        fprintf(stderr, "written %ld, expected %ld\n", r,
                                p->size);
                        exit(EXIT_FAILURE);
                    }
                }

                assert(p->package_type == CLOSE_PACKAGE || child_finished_flag);
                if (p->package_type == CLOSE_PACKAGE) {
                    kill(executor_process, SIGKILL);
                }

                close(connection);

                exit(EXIT_SUCCESS);

            } else {
                // executor

                int client_socket_out = dup(connection);
                // dup out to socket
                dup2(client_socket_out, STDOUT_FILENO);
                dup2(client_socket_out, STDERR_FILENO);
                // dup in from pipe from package_handler
                dup2(fd_write_handler[0], STDIN_FILENO);

                close(connection);
                close(client_socket_out);
                // execvp(cmd, args)
                execvp(op->cmd, op->args);

                fprintf(stderr, "execvp fail\n");
                exit(EXIT_FAILURE);
            }
        }

        close(connection);
    }
}
