/** client.c —— 基于数据报套接字（UDP）的聊天客户端 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define BUF_SIZE 1024 /* 收发缓冲区大小（字节） */

/* 去掉 fgets 读入行末尾的 '\n' 或 '\r'，
 * 否则输入的 "bye" 与程序里的 "bye" 比较时不会相等。 */
void trim(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    {
        n--;
        s[n] = '\0';
    }
}

int main()
{
    int sockfd;                     /* 套接字描述符 */
    struct sockaddr_in server_addr; /* 目标服务器的地址信息 */
    socklen_t server_len;           /* 地址结构长度 */
    char buf[BUF_SIZE];
    ssize_t n;                      /* recvfrom 的返回值 */
    char ip[64], port_str[16];      /* 键盘输入的 IP 与端口字符串 */

    /* 1. 创建数据报套接字。
     *    SOCK_DGRAM 表示 UDP；客户端不需要 bind()，
     *    第一次 sendto() 时内核会自动分配一个临时端口。 */
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* 2. 从键盘输入服务器 IP 地址 */
    printf("Input server ip: ");
    if (fgets(ip, sizeof(ip), stdin) == NULL)
    {
        close(sockfd);
        return 0;
    }
    trim(ip);

    /* 从键盘输入服务器端口号（此时还是字符串） */
    printf("Input server port: ");
    if (fgets(port_str, sizeof(port_str), stdin) == NULL)
    {
        close(sockfd);
        return 0;
    }
    trim(port_str);

    /* 3. 填写服务器地址结构 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;

    /* inet_addr 把 "127.0.0.1" 这样的点分十进制字符串
     * 转成 32 位二进制地址，返回值已经是网络字节序 */
    server_addr.sin_addr.s_addr = inet_addr(ip);

    /* atoi 把端口字符串转成整数，htons 再转成网络字节序；
     * 端口号只有 16 位，所以强制转换为 unsigned short */
    server_addr.sin_port = htons((unsigned short)atoi(port_str));

    server_len = sizeof(server_addr);

    /* 4. 循环：从键盘读一行发送，然后等待服务器的回复 */
    for (;;)
    {
        printf("Input message (bye to quit): ");
        if (fgets(buf, BUF_SIZE, stdin) == NULL)
            break;

        trim(buf);

        /* 用 sendto 直接指定目的地址，UDP 不需要先 connect。
         * 发送长度取 strlen(buf)，所以数据报中不含行尾换行符。 */
        if (sendto(sockfd, buf, strlen(buf), 0,
                   (struct sockaddr *)&server_addr, server_len) < 0)
        {
            perror("sendto");
            break;
        }

        /* 客户端输入 "bye"：本客户端退出。
         * 服务器收到 bye 后只结束本次会话，会继续等待其他客户端，
         * 因此客户端发送后不必再等待回复。 */
        if (strcmp(buf, "bye") == 0)
        {
            printf("Client said bye, exit.\n");
            break;
        }

        /* 等待服务器回复。第 5、6 个参数传 NULL，
         * 表示不关心这个数据报来自哪个地址。 */
        n = recvfrom(sockfd, buf, BUF_SIZE - 1, 0, NULL, NULL);

        if (n < 0)
        {
            perror("recvfrom");
            break;
        }

        buf[n] = '\0';
        trim(buf);

        printf("Recv from server: %s\n", buf);

        /* 服务器发来 "bye"：说明服务器要退出，
         * 客户端收到后也结束运行。 */
        if (strcmp(buf, "bye") == 0)
        {
            printf("Server said bye, client exit.\n");
            break;
        }
    }

    close(sockfd); /* 释放套接字资源 */

    return 0;
}
