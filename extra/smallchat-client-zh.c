/* smallchat-client.c -- smallchat-server的客户端程序。
 *
 * Copyright (c) 2023，Salvatore Sanfilippo <antirez at gmail dot com>
 * 保留所有权利。
 *
 * 在源代码和二进制形式中进行再发布和使用，无论是否
 * 修改，都被允许，前提是满足以下条件：
 *
 *   * 必须保留源代码的上述版权声明、
 *     此条件列表和以下免责声明。
 *   * 在二进制形式中再发布时，必须在
 *     与分发一起提供上述版权声明、此条件和以下免责声明的
 *     文件、文档和/或其他材料。
 *   * 不能使用项目名称或其贡献者的名称
 *     为此软件派生的产品背书或推广此软件
 *     没有特定的事先书面许可。
 *
 * 此软件由版权所有者和贡献者“按原样”提供，
 * 任何明示或暗示的保证，包括但不限于
 * 对适销性和特定用途的适用性的保证。
 * 在任何情况下，版权所有者或贡献者均不对任何直接、间接、
 * 偶然、特殊、典型或因使用而引起的损害或
 * 其他责任（包括但不限于，采购
 * 替代品或服务; 使用数据、利润或业务中断），
 * 无论是在合同、严格责任或侵权（包括疏忽或其他）
 * 方式）引起的任何理论，无论是否通知了此类损害的可能性。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/select.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#include "chatlib.h"

/* ============================================================================
 * 低级终端处理。
 * ========================================================================== */

void disableRawModeAtExit(void);

/* 原始模式：1960年的魔法。 */
int setRawMode(int fd, int enable) {
    /* 我们有一些全局状态（但是在范围内）。
     * 这是为了正确设置/撤消原始模式。 */
    static struct termios orig_termios; // 将原始终端状态保存在这里。
    static int atexit_registered = 0;   // 避免多次注册atexit()。
    static int rawmode_is_set = 0;      // 如果启用了原始模式，则为真。

    struct termios raw;

    /* 如果enable为零，我们只需在当前设置的情况下禁用原始模式，如果它当前已设置。 */
    if (enable == 0) {
        /* 即使返回值太迟，也不要检查，因为现在太迟了。 */
        if (rawmode_is_set && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
            rawmode_is_set = 0;
        return 0;
    }

    /* 启用原始模式。 */
    if (!isatty(fd)) goto fatal;
    if (!atexit_registered) {
        atexit(disableRawModeAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* 修改原始模式 */
    /* 输入模式：没有中断、没有CR到NL、没有奇偶校验、没有剥离字符、
     * 没有开始/停止输出控制。 */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* 输出模式 - 什么也不做。我们希望启用后处理，以便
     * \n将自动转换为\r\n。 */
    // raw.c_oflag &= ...
    /* 控制模式 - 设置8位字符 */
    raw.c_cflag |= (CS8);
    /* 本地模式 - 关闭回显、规范关闭、没有扩展功能，
     * 但是启用了信号字符（^Z，^C）。 */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    /* 控制字符 - 设置返回条件：最小字节数和定时器。
     * 我们希望读取每个字节，而不带超时。 */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1字节，无定时器 */

    /* 在刷新后将终端放入原始模式 */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    rawmode_is_set = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

/* 在退出时我们将尝试将终端修复到初始条件。 */
void disableRawModeAtExit(void) {
    setRawMode(STDIN_FILENO,0);
}

/* ============================================================================
 * 最小行编辑。
 * ========================================================================== */

void terminalCleanCurrentLine(void) {
    write(fileno(stdout),"\e[2K",4);
}

void terminalCursorAtLineStart(void) {
    write(fileno(stdout),"\r",1);
}

#define IB_MAX 128
struct InputBuffer {
    char buf[IB_MAX];       // 保存数据的缓冲区。
    int len;                // 当前长度。
};

/* inputBuffer*() 返回值: */
#define IB_ERR 0        // 对不起，无法满足。
#define IB_OK 1         // 好的，得到新的字符，执行操作，...
#define IB_GOTLINE 2    // 嘿，现在有一个格式良好的行可读。

/* 将指定的字符附加到缓冲区。 */
int inputBufferAppend(struct InputBuffer *ib, int c) {
    if (ib->len >= IB_MAX) return IB_ERR; // 没有空间。

    ib->buf[ib->len] = c;
    ib->len++;
    return IB_OK;
}

void inputBufferHide(struct InputBuffer *ib);
void inputBufferShow(struct InputBuffer *ib);

/* 处理来自键盘的每个新键击。作为副作用，
 * 为了反映用户当前正在键入的当前行，修改输入缓冲区状态，
 * 以便读取输入缓冲区'buf'的'len'字节将其包含。 */
int inputBufferFeedChar(struct InputBuffer *ib, int c) {
    switch(c) {
    case '\n':
        break;          // 被忽略。我们处理\r而不是。
    case '\r':
        return IB_GOTLINE;
    case 127:           // 退格。
        if (ib->len > 0) {
            ib->len--;
            inputBufferHide(ib);
            inputBufferShow(ib);
        }
        break;
    default:
        if (inputBufferAppend(ib,c) == IB_OK)
            write(fileno(stdout),ib->buf+ib->len-1,1);
        break;
    }
    return IB_OK;
}

/* 隐藏用户正在输入的行。 */
void inputBufferHide(struct InputBuffer *ib) {
    (void)ib; // 未使用的变量，但在概念上属于API的一部分。
    terminalCleanCurrentLine();
    terminalCursorAtLineStart();
}

/* 再次显示当前行。通常在InputBufferHide()之后调用。 */
void inputBufferShow(struct InputBuffer *ib) {
    write(fileno(stdout),ib->buf,ib->len);
}

/* 将缓冲区重置为空。 */
void inputBufferClear(struct InputBuffer *ib) {
    ib->len = 0;
    inputBufferHide(ib);
}

/* =============================================================================
 * 主程序逻辑，最后 :)
 * ========================================================================== */

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }

    /* 与服务器创建TCP连接。 */
    int s = TCPConnect(argv[1],atoi(argv[2]),0);
    if (s == -1) {
        perror("Connecting to server");
        exit(1);
    }

    /* 将终端设置为原始模式：这样我们将收到每个
     * 用户键入时的单个按键。没有任何缓冲
     * 或转换任何类型的转义序列。 */
    setRawMode(fileno(stdin),1);

    /* 等待标准输入或服务器套接字
     * 有一些数据。 */
    fd_set readfds;
    int stdin_fd = fileno(stdin);

    struct InputBuffer ib;
    inputBufferClear(&ib);

    while(1) {
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        FD_SET(stdin_fd, &readfds);
        int maxfd = s > stdin_fd ? s : stdin_fd;

        int num_events = select(maxfd+1, &readfds, NULL, NULL, NULL);
        if (num_events == -1) {
            perror("select() error");
            exit(1);
        } else if (num_events) {
            char buf[128]; /* 用于两种代码路径的通用缓冲区。 */

            if (FD_ISSET(s, &readfds)) {
                /* 来自服务器的数据？ */
                ssize_t count = read(s,buf,sizeof(buf));
                if (count <= 0) {
                    printf("Connection lost\n");
                    exit(1);
                }
                inputBufferHide(&ib);
                write(fileno(stdout),buf,count);
                inputBufferShow(&ib);
            } else if (FD_ISSET(stdin_fd, &readfds)) {
                /* 来自用户在终端上键入的数据？ */
                ssize_t count = read(stdin_fd,buf,sizeof(buf));
                for (int j = 0; j < count; j++) {
                    int res = inputBufferFeedChar(&ib,buf[j]);
                    switch(res) {
                    case IB_GOTLINE:
                        inputBufferAppend(&ib,'\n');
                        inputBufferHide(&ib);
                        write(fileno(stdout),"you> ", 5);
                        write(fileno(stdout),ib.buf,ib.len);
                        write(s,ib.buf,ib.len);
                        inputBufferClear(&ib);
                        break;
                    case IB_OK:
                        break;
                    }
                }
            }
        }
    }

    close(s);
    return 0;
}
