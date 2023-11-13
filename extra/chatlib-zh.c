#define _POSIX_C_SOURCE 200112L
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================== 低级网络功能 ==========================
 * 这里包含了基本的套接字操作，这些操作应该是一个良好的标准C库的一部分，
 * 但你知道... 还有其他疯狂的目标，比如把整个语言都变成未定义行为。
 * =========================================================================== */

/* 将指定的套接字设置为非阻塞模式，不延迟标志。 */
int socketSetNonBlockNoDelay(int fd) {
    int flags, yes = 1;

    /* 将套接字设置为非阻塞。
     * 请注意，fcntl(2)用于F_GETFL和F_SETFL的调用不能被信号中断。*/
    if ((flags = fcntl(fd, F_GETFL)) == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;

    /* 这是最好的努力。不需要检查错误。 */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return 0;
}

/* 创建一个TCP套接字，监听 'port' 准备接受连接。 */
int createTCPServer(int port) {
    int s, yes = 1;
    struct sockaddr_in sa;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); // 最佳努力。

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) == -1 ||
        listen(s, 511) == -1)
    {
        close(s);
        return -1;
    }
    return s;
}

/* 创建一个TCP套接字并将其连接到指定的地址。
 * 成功时返回套接字描述符，否则返回-1。
 *
 * 如果 'nonblock' 非零，将套接字置于非阻塞状态，并且connect()尝试不会阻塞，
 * 但套接字可能不会立即准备好写入。 */
int TCPConnect(char *addr, int port, int nonblock) {
    int s, retval = -1;
    struct addrinfo hints, *servinfo, *p;

    char portstr[6]; /* 最大16位数字符串长度。 */
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(addr, portstr, &hints, &servinfo) != 0) return -1;

    for (p = servinfo; p != NULL; p = p->ai_next) {
        /* 尝试创建套接字并连接它。
         * 如果在socket()调用中失败，或在connect()中失败，我们将在servinfo中的下一个条目中重试。 */
        if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
            continue;

        /* 如果需要，将其置于非阻塞状态。 */
        if (nonblock && socketSetNonBlockNoDelay(s) == -1) {
            close(s);
            break;
        }

        /* 尝试连接。 */
        if (connect(s, p->ai_addr, p->ai_addrlen) == -1) {
            /* 如果套接字是非阻塞的，在这里返回EINPROGRESS错误是可以的。 */
            if (errno == EINPROGRESS && nonblock) return s;

            /* 否则是一个错误。 */
            close(s);
            break;
        }

        /* 如果我们在for循环的迭代中没有错误地结束，我们有一个已连接的套接字。让我们返回给调用者。 */
        retval = s;
        break;
    }

    freeaddrinfo(servinfo);
    return retval; /* 如果没有连接成功，则为-1。 */
}

/* 如果监听套接字通知有一个新连接准备好接受，我们accept(2)它并在错误时返回-1，成功时返回新的客户端套接字。 */
int acceptClient(int server_socket) {
    int s;

    while (1) {
        struct sockaddr_in sa;
        socklen_t slen = sizeof(sa);
        s = accept(server_socket, (struct sockaddr*)&sa, &slen);
        if (s == -1) {
            if (errno == EINTR)
                continue; /* 重试。 */
            else
                return -1;
        }
        break;
    }
    return s;
}

/* 我们还定义了一个总是在内存不足时崩溃的分配器：
 * 你会发现，在设计成长时间运行的程序时，尝试从内存不足中恢复通常是徒劳的，同时也会使整个程序变得糟糕。 */
void *chatMalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        perror("内存不足");
        exit(1);
    }
    return ptr;
}

/* 同样的，中止realloc()。 */
void *chatRealloc(void *ptr, size_t size) {
    ptr = realloc(ptr, size);
    if (ptr == NULL) {
        perror("内存不足");
        exit(1);
    }
    return ptr;
}
