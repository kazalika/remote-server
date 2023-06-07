#include <assert.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

enum { BUF_SIZE = 10 };

// package type
enum {
    DATA_PACKAGE = 0,
    OPEN_PACKAGE = 1,
    CLOSE_PACKAGE = 2,
};

int create_connection(char *node, char *service) {
    struct addrinfo *res = NULL;
    int gai_err;
    struct addrinfo hint = {
        .ai_family = AF_UNSPEC,  // можно и AF_INET, и AF_INET6
        .ai_socktype = SOCK_STREAM,  // но мы хотим поток (соединение)
    };
    if ((gai_err = getaddrinfo(node, service, &hint, &res))) {
        fprintf(stderr, "gai error: %s\n", gai_strerror(gai_err));
        return -1;
    }
    int sock = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, 0);
        if (sock < 0) {
            perror("socket");
            continue;
        }
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
            perror("connect");
            close(sock);
            sock = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    return sock;
}

_Atomic int is_reading = 1;
int sock_in, sock_out;

// OPEN_PACKAGE structure
// args_count  len(cmd)  cmd    len(args[0])  args[0]  ...
// len(args[args_count-1])  args[args_count-1] size_t      size_t    chars
// size_t        chars    ... size_t                   chars
int send_open_package(int argc, char *cmd, char *argv[]) {
    ssize_t r;

    size_t args_count = argc;
    size_t len_cmd = strlen(cmd);

    r = write(sock_out, &args_count, sizeof(args_count));
    assert(r == sizeof(args_count));

    r = write(sock_out, &len_cmd, sizeof(len_cmd));
    assert(r == sizeof(len_cmd));

    r = write(sock_out, cmd, len_cmd);
    assert(r == len_cmd);

    for (size_t i = 0; i < args_count; ++i) {
        size_t len_arg = strlen(argv[i]);
        r = write(sock_out, &len_arg, sizeof(len_arg));
        assert(r == sizeof(len_arg));

        r = write(sock_out, argv[i], len_arg);
        assert(r == len_arg);
    }
    return 0;
}

// PACKAGE structure
// package_type  size    data
// int           size_t  chars
int send_package(int package_type, size_t size, char *buf) {
    ssize_t r;

    r = write(sock_out, &package_type, sizeof(package_type));
    assert(r == sizeof(package_type));

    r = write(sock_out, &size, sizeof(size));
    assert(r == sizeof(size));

    r = write(sock_out, buf, size);
    assert(r == size);

    return 0;
}

void *read_thread(void *args) {
    char buf[BUF_SIZE];
    ssize_t r;

    while (atomic_load(&is_reading) &&
           (r = read(STDIN_FILENO, buf, BUF_SIZE)) > 0) {
        send_package(DATA_PACKAGE, r, buf);
    }

    send_package(CLOSE_PACKAGE, 1, buf);

    return NULL;
}

void *write_thread(void *args) {
    char buf[BUF_SIZE];
    ssize_t r;
    while ((r = read(sock_in, buf, BUF_SIZE)) > 0) {
        // printf("read from server %ld symbols\n", r);

        ssize_t q = write(STDOUT_FILENO, buf, r);
        assert(q == r);
    }

    exit(EXIT_SUCCESS);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <ip> <port> spawn <command> [args ...]\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }
    sock_in = create_connection(argv[1], argv[2]);
    if (sock_in < 0) {
        fprintf(stderr, "create_connection failed\n");
        exit(EXIT_FAILURE);
    }

    sock_out = dup(sock_in);

    // send open_package

    send_open_package(argc - 5, argv[4], argv + 5);

    pthread_t pread, pwrite;
    pthread_create(&pwrite, NULL, *write_thread, NULL);
    pthread_create(&pread, NULL, *read_thread, NULL);

    pthread_join(pread, NULL);
    pthread_join(pwrite, NULL);
}
