#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/select.h>
#include <unistd.h>

#include "chatlib.h"

/* ============================ 数据结构 =================================
 * 这是一个简单的聊天程序，我们希望代码简单易懂，即使对于不熟悉C语言的人也能理解。
 * =========================================================================== */

#define MAX_CLIENTS 1000 // 这实际上是最高文件描述符。
#define SERVER_PORT 7711

/* 这个结构表示一个已连接的客户端。关于客户端的信息很少：套接字描述符和昵称（如果设置），否则昵称的第一个字节设置为0（如果未设置）。
客户端可以使用/nick <nickname>命令设置其昵称。 */
struct client {
    int fd;     // 客户端套接字。
    char *nick; // 客户端的昵称。
};

/* 这个全局结构封装了聊天的全局状态。 */
struct chatState {
    int serversock;     // 用于监听的服务器套接字。
    int numclients;     // 当前连接的客户端数。
    int maxclient;      // 占用的最大“clients”插槽。
    struct client *clients[MAX_CLIENTS]; // 客户端被设置在其套接字描述符的相应插槽中。
};

struct chatState *Chat; // 在启动时初始化。

/* ====================== 小型聊天核心实现 ========================
 * 这里的想法非常简单：我们接受新的连接，读取客户端写给我们的消息，并将消息传播（即，发送到所有其他连接的客户端），除了发送者。当然，这是可能的最简单的聊天系统。
 * =========================================================================== */

/* 创建一个绑定到'fd'的新客户端。当新客户端连接时调用此函数。作为副作用，更新全局Chat状态。 */
struct client *createClient(int fd) {
    char nick[32]; // 用于为用户创建初始昵称。
    int nicklen = snprintf(nick, sizeof(nick), "user:%d", fd);
    struct client *c = chatMalloc(sizeof(*c));
    socketSetNonBlockNoDelay(fd); // 假装这不会失败。
    c->fd = fd;
    c->nick = chatMalloc(nicklen + 1);
    memcpy(c->nick, nick, nicklen);
    assert(Chat->clients[c->fd] == NULL); // 这应该是可用的。
    Chat->clients[c->fd] = c;
    /* 如果需要，我们需要更新max client set。 */
    if (c->fd > Chat->maxclient) Chat->maxclient = c->fd;
    Chat->numclients++;
    return c;
}

/* 释放客户端、关联资源并从Chat的全局状态中解绑。 */
void freeClient(struct client *c) {
    free(c->nick);
    close(c->fd);
    Chat->clients[c->fd] = NULL;
    Chat->numclients--;
    if (Chat->maxclient == c->fd) {
        /* 哎呀，这是max client set。让我们找到什么是
         * 新的最高插槽使用。 */
        int j;
        for (j = Chat->maxclient - 1; j >= 0; j--) {
            if (Chat->clients[j] != NULL) {
                Chat->maxclient = j;
                break;
            }
        }
        if (j == -1) Chat->maxclient = -1; // 我们不再有客户端。
    }
    free(c);
}

/* 分配并初始化全局变量。 */
void initChat(void) {
    Chat = chatMalloc(sizeof(*Chat));
    memset(Chat, 0, sizeof(*Chat));
    /* 启动时没有客户端。 */
    Chat->maxclient = -1;
    Chat->numclients = 0;

    /* 创建我们的监听套接字，绑定到给定的端口。这
     * 是我们的客户端将连接的地方。 */
    Chat->serversock = createTCPServer(SERVER_PORT);
    if (Chat->serversock == -1) {
        perror("Creating listening socket");
        exit(1);
    }
}

/* 将指定的字符串发送给除了具有套接字描述符'excluded'的客户端之外的所有连接客户端。如果要发送一些东西
 * 给每个客户端，只需将excluded设置为不可能的套接字：-1。 */
void sendMsgToAllClientsBut(int excluded, char *s, size_t len) {
    for (int j = 0; j <= Chat->maxclient; j++) {
        if (Chat->clients[j] == NULL ||
            Chat->clients[j]->fd == excluded) continue;

        /* 重要：我们不做任何缓冲。我们只使用内核
         * 套接字缓冲区。如果内容不适合，我们不关心。
         * 为了保持此程序的简单性，这是必需的。 */
        write(Chat->clients[j]->fd, s, len);
    }
}

/* 主()函数实现主要聊天逻辑：
 * 1. 接受新的客户端连接（如果有）。
 * 2. 检查是否有任何客户端发送给我们的新消息。
 * 3. 将消息发送给所有其他客户端。 */
int main(void) {
    initChat();

    while (1) {
        fd_set readfds;
        struct timeval tv;
        int retval;

        FD_ZERO(&readfds);
        /* 当我们希望通过select()通知有
         * 活动？如果监听套接字有待接受的挂起
         * 或者如果任何其他客户端写了什么。 */
        FD_SET(Chat->serversock, &readfds);

        for (int j = 0; j <= Chat->maxclient; j++) {
            if (Chat->clients[j]) FD_SET(j, &readfds);
        }

        /* 为select()设置超时，稍后看到为什么这可能是有用的
         * 将来（现在不是）。 */
        tv.tv_sec = 1; // 1秒超时
        tv.tv_usec = 0;

        /* Select希望作为第一个参数的最大文件描述符
         * 在使用的基础上加一。它可以是我们的任何客户端之一，也可以是
         * 服务器套接字本身。 */
        int maxfd = Chat->maxclient;
        if (maxfd < Chat->serversock) maxfd = Chat->serversock;
        retval = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (retval == -1) {
            perror("select() error");
            exit(1);
        } else if (retval) {

            /* 如果监听套接字是“可读”的，实际上意味着
             * 有新的客户端连接待处理。 */
            if (FD_ISSET(Chat->serversock, &readfds)) {
                int fd = acceptClient(Chat->serversock);
                struct client *c = createClient(fd);
                /* 发送欢迎消息。 */
                char *welcome_msg =
                    "Welcome to Simple Chat! "
                    "Use /nick <nick> to set your nick.\n";
                write(c->fd, welcome_msg, strlen(welcome_msg));
                printf("Connected client fd=%d\n", fd);
            }

            /* 这里对于每个已连接的客户端，检查是否有挂起的
             * 数据客户端发送给我们。 */
            char readbuf[256];
            for (int j = 0; j <= Chat->maxclient; j++) {
                if (Chat->clients[j] == NULL) continue;
                if (FD_ISSET(j, &readfds)) {
                    /* 在这里，我们只是希望有一个格式良好的
                     * 消息等着我们。但是完全可能
                     * 我们只读了一半的消息。在一个正常的程序中
                     * 这不是设计为如此简单，我们应该尝试
                     * 缓冲读取直到达到行的末尾。 */
                    int nread = read(j, readbuf, sizeof(readbuf) - 1);

                    if (nread <= 0) {
                        /* 错误或短读取意味着套接字
                         * 已关闭。 */
                        printf("Disconnected client fd=%d, nick=%s\n",
                               j, Chat->clients[j]->nick);
                        freeClient(Chat->clients[j]);
                    } else {
                        /* 客户端发送给我们一条消息。我们需要
                         * 将此消息转发给聊天中的所有其他客户端
                         * 除了发送者。 */
                        struct client *c = Chat->clients[j];
                        readbuf[nread] = 0;

                        /* 如果用户消息以“/”开头，我们
                         * 将其处理为客户端命令。到目前为止
                         * 只实现了/nick <newnick>命令。 */
                        if (readbuf[0] == '/') {
                            /* 删除任何尾随换行符。 */
                            char *p;
                            p = strchr(readbuf, '\r');
                            if (p) *p = 0;
                            p = strchr(readbuf, '\n');
                            if (p) *p = 0;
                            /* 检查命令的参数，在空格之后。 */
                            char *arg = strchr(readbuf, ' ');
                            if (arg) {
                                *arg = 0; /* 终止命令名称。 */
                                arg++;   /* 参数在空格之后1字节。 */
                            }

                            if (!strcmp(readbuf, "/nick") && arg) {
                                free(c->nick);
                                int nicklen = strlen(arg);
                                c->nick = chatMalloc(nicklen + 1);
                                memcpy(c->nick, arg, nicklen + 1);
                            } else {
                                /* 不支持的命令。发送错误。 */
                                char *errmsg = "Unsupported command\n";
                                write(c->fd, errmsg, strlen(errmsg));
                            }
                        } else {
                            /* 创建要发送给所有人的消息（并显示
                             * 在服务器控制台上）的形式：
                             *   nick> some message。 */
                            char msg[256];
                            int msglen = snprintf(msg, sizeof(msg),
                                                 "%s> %s", c->nick, readbuf);

                            /* snprintf()的返回值可能大于
                             * sizeof(msg)，如果没有足够的空间
                             * 完整的输出。 */
                            if (msglen >= (int)sizeof(msg))
                                msglen = sizeof(msg) - 1;
                            printf("%s", msg);

                            /* 将其发送给所有其他客户端。 */
                            sendMsgToAllClientsBut(j, msg, msglen);
                        }
                    }
                }
            }
        } else {
            /* 超时发生。我们现在什么也不做，但是通常这一部分可以用来定期唤醒，即使没有客户端活动。 */
        }
    }
    return 0;
}
