#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#ifdef _WIN32

#include <ws2tcpip.h>
#include <io.h>
#include <direct.h>

#define isatty _isatty
#define fileno _fileno
#define unlink _unlink
#define chmod _chmod

inline ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (lineptr == NULL || n == NULL || stream == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (*lineptr == NULL) {
            errno = ENOMEM;
            return -1;
        }
    }
    size_t count = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (count + 1 >= *n) {
            size_t new_len = *n * 2;
            char *new_ptr = (char *)realloc(*lineptr, new_len);
            if (new_ptr == NULL) {
                errno = ENOMEM;
                return -1;
            }
            *lineptr = new_ptr;
            *n = new_len;
        }
        (*lineptr)[count++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    if (count == 0 && c == EOF) {
        return -1;
    }
    (*lineptr)[count] = '\0';
    return (ssize_t)count;
}

inline void win32_init_sockets() {
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", res);
        exit(1);
    }
}

#else

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

inline void win32_init_sockets() {}

#endif
