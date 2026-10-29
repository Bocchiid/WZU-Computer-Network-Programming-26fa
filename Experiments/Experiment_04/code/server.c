/** server.c —— 基于数据报套接字（UDP）的聊天服务器 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define SERVER_PORT 8888 /* 服务器监听的端口号 */
#define BUF_SIZE 1024    /* 收发缓冲区大小（字节） */

/* 去掉 fgets 读入行末尾的 '\n' 或 '\r'。
 * 如果不删除，输入 "bye\n" 与 "bye" 比较时永远不相等，
 * 退出逻辑就不会生效。 */
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
    int sockfd;                     /* 套接字描述符，TCP/UDP 都用它 */
    struct sockaddr_in server_addr; /* 服务器自己的地址信息 */
    struct sockaddr_in client_addr; /* 保存 recvfrom 得到的客户端地址 */
    socklen_t client_len;           /* 地址结构的长度（入口/出口参数） */
    char buf[BUF_SIZE];
    ssize_t n; /* recvfrom 的返回值：收到的字节数 */

    /* 1. 创建套接字。
     *    AF_INET 表示 IPv4；SOCK_DGRAM 表示数据报套接字，即 UDP。
     *    第三个参数为 0，表示使用该类型对应的默认协议。 */
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* 2. 填写服务器地址结构：协议族、IP 地址、端口号 */
    memset(&server_addr, 0, sizeof(server_addr)); /* 先清零，避免残留脏数据 */

    server_addr.sin_family = AF_INET; /* 地址族：IPv4 */

    /* INADDR_ANY 表示绑定本机所有网卡（0.0.0.0），
     * htonl 把主机字节序的 32 位整数转成网络字节序 */
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* htons 把主机字节序的 16 位端口转成网络字节序 */
    server_addr.sin_port = htons(SERVER_PORT);

    /* 3. 绑定本地地址和端口。
     *    UDP 是无连接的，不需要 listen() 和 accept()，
     *    bind() 成功后其他主机就可以向这个端口发送数据报。 */
    if (bind(sockfd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    printf("Server listening on port %d\n", SERVER_PORT);

    /* 4. 循环：接收一个数据报，再向发送方回复一个数据报 */
    for (;;)
    {
        /* 每次调用 recvfrom 前都要把 client_len 重置为缓冲区大小。
         * client_len 既是入口参数也是出口参数，上一次调用会把它
         * 改写成实际地址长度，不重置可能影响下一次接收。 */
        client_len = sizeof(client_addr);

        /* 接收数据报。第 5、6 个参数是出口参数：
         * 内核会把发送方的 IP 和端口填入 client_addr，
         * 服务器正是通过它们得到客户端的通信端点地址。
         * 缓冲区长度用 BUF_SIZE - 1，给字符串结束符留一个字节。 */
        n = recvfrom(sockfd, buf, BUF_SIZE - 1, 0,
                     (struct sockaddr *)&client_addr, &client_len);

        if (n < 0)
        {
            perror("recvfrom");
            continue; /* 单个报文出错不影响服务器继续运行 */
        }

        buf[n] = '\0'; /* 数据报是原始字节，手动补上结束符 */
        trim(buf);     /* 去掉行尾可能存在的 '\n'、'\r' */

        /* 显示消息内容和来源。
         * inet_ntoa 把 32 位网络字节序地址转成点分十进制字符串；
         * ntohs 把网络字节序的端口还原为主机字节序。 */
        printf("Recv from %s:%d : %s\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port), buf);

        /* 客户端发来 "bye"：只结束与该客户端的会话。
         * 服务器不回复，继续等待下一个客户端。 */
        if (strcmp(buf, "bye") == 0)
        {
            printf("Client %s:%d said bye, continue waiting\n",
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));
            continue;
        }

        /* 从键盘读入一行，作为给客户端的回复 */
        printf("Input reply (bye to quit): ");
        if (fgets(buf, BUF_SIZE, stdin) == NULL)
            break; /* 标准输入结束（如 Ctrl+D），退出循环 */
        trim(buf);

        /* 把回复发送给刚才那个客户端。
         * 目的地址必须使用 recvfrom 填好的 client_addr，
         * 长度使用本次得到的 client_len。 */
        if (sendto(sockfd, buf, strlen(buf), 0,
                   (struct sockaddr *)&client_addr, client_len) < 0)
        {
            perror("sendto");
            continue;
        }

        /* 服务器操作员输入 "bye"：该回复已经发给客户端，
         * 客户端收到后也会退出，服务器随后退出。 */
        if (strcmp(buf, "bye") == 0)
        {
            printf("Server said bye, exit.\n");
            break;
        }
    }

    close(sockfd); /* 释放套接字资源 */

    return 0;
}
