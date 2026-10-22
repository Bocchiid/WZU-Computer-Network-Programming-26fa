/* server.c —— 实验3 基于流式套接字的文件传输程序（服务器端）
 *
 * 编译：gcc server.c -o server -Wall
 * 运行：./server [端口] [每64KB延时毫秒]
 *       端口默认 8888；第二个参数只用于演示断点续传时“放慢”发送速度，正常使用省略。
 *
 * 协议：
 *   客户端先发 21 字节请求头 struct file_request，再发 name_len 字节的文件名。
 *   服务器回一个 8 字节 int64：>=0 表示文件总长度（上传时表示接受的长度），<0 表示错误。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DEFAULT_PORT   8888
#define BUF_SIZE       (64 * 1024)
#define MAX_NAME_LEN   4096

/* 请求头：固定 21 字节。用 packed 避免编译器插入填充字节，
 * 保证收发双方对报文布局的理解完全一致。 */
struct file_request {
    uint8_t  cmd;        /* 'D' = 下载，'U' = 上传 */
    uint32_t name_len;   /* 文件名字节数，网络字节序 */
    uint64_t offset;     /* 起始偏移量，网络字节序 */
    uint64_t file_size;  /* 文件总长度，网络字节序；仅上传时有效 */
} __attribute__((packed));

static int throttle_ms = 0;   /* 仅演示用：每发送 64KB 暂停的毫秒数 */

/* 64 位主机字节序 -> 网络字节序（大端） */
static uint64_t hton64(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(v & 0xffffffffu)) << 32) |
           (uint64_t)htonl((uint32_t)(v >> 32));
#else
    return v;
#endif
}
#define ntoh64 hton64   /* 转换是对称的 */

/* 收满 n 字节：1=成功，0=对端关闭，-1=出错 */
static int recv_all(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = recv(fd, (char *)buf + done, n - done, 0);
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)r;
    }
    return 1;
}

/* 发满 n 字节：1=成功，-1=出错 */
static int send_all(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = send(fd, (const char *)buf + done, n - done, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 1;
}

static int write_all(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

/* 发送 8 字节的 int64 响应 */
static int send_i64(int fd, int64_t value)
{
    uint64_t net = hton64((uint64_t)value);
    return send_all(fd, &net, sizeof net);
}

/* ---------- 下载：把服务器上的文件发给客户端 ---------- */
static void handle_download(int conn, const char *path, uint64_t offset)
{
    char buf[BUF_SIZE];
    struct stat st;
    uint64_t remain, sent = 0;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("打开文件 %s 失败：%s\n", path, strerror(errno));
        send_i64(conn, -1);
        return;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        printf("无法获取文件 %s 的信息\n", path);
        send_i64(conn, -2);
        close(fd);
        return;
    }
    if (offset > (uint64_t)st.st_size) {
        printf("偏移 %llu 超过文件大小 %lld\n",
               (unsigned long long)offset, (long long)st.st_size);
        send_i64(conn, -3);
        close(fd);
        return;
    }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        perror("lseek");
        send_i64(conn, -2);
        close(fd);
        return;
    }

    /* 先告诉客户端：文件存在，总长度是 st_size */
    if (send_i64(conn, (int64_t)st.st_size) != 1) {
        close(fd);
        return;
    }
    printf("下载请求：%s，文件大小 %lld 字节，起始偏移 %llu\n",
           path, (long long)st.st_size, (unsigned long long)offset);

    remain = (uint64_t)st.st_size - offset;
    while (remain > 0) {
        size_t want = remain > BUF_SIZE ? BUF_SIZE : (size_t)remain;
        ssize_t r = read(fd, buf, want);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        }
        if (r == 0)
            break;
        if (send_all(conn, buf, (size_t)r) != 1) {
            printf("客户端已断开，发送停止\n");
            close(fd);
            return;
        }
        sent += (uint64_t)r;
        remain -= (uint64_t)r;
        if (throttle_ms > 0)
            usleep((useconds_t)throttle_ms * 1000);
    }
    close(fd);

    if (remain == 0)
        printf("发送完成，本次共发送 %llu 字节\n", (unsigned long long)sent);
    else
        printf("发送中断，已发送 %llu 字节\n", (unsigned long long)sent);
}

/* ---------- 上传：接收客户端发来的文件 ---------- */
static void handle_upload(int conn, const char *path, uint64_t offset,
                          uint64_t total)
{
    char buf[BUF_SIZE];
    uint64_t remain, got = 0;
    int flags, fd;

    if (offset > total) {
        send_i64(conn, -3);
        return;
    }

    /* offset == 0 表示从头传，覆盖旧文件；offset > 0 表示续传，保留已有内容 */
    flags = O_WRONLY | O_CREAT;
    if (offset == 0)
        flags |= O_TRUNC;
    fd = open(path, flags, 0644);
    if (fd < 0) {
        printf("创建文件 %s 失败：%s\n", path, strerror(errno));
        send_i64(conn, -1);
        return;
    }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        perror("lseek");
        send_i64(conn, -2);
        close(fd);
        return;
    }

    if (send_i64(conn, (int64_t)total) != 1) {
        close(fd);
        return;
    }
    printf("上传请求：%s，文件大小 %llu 字节，起始偏移 %llu\n",
           path, (unsigned long long)total, (unsigned long long)offset);

    remain = total - offset;
    while (remain > 0) {
        size_t want = remain > BUF_SIZE ? BUF_SIZE : (size_t)remain;
        ssize_t r = recv(conn, buf, want, 0);
        if (r == 0) {
            printf("客户端已断开，上传未完成\n");
            break;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            break;
        }
        if (write_all(fd, buf, (size_t)r) < 0) {
            perror("write");
            break;
        }
        got += (uint64_t)r;
        remain -= (uint64_t)r;
        if (throttle_ms > 0)
            usleep((useconds_t)throttle_ms * 1000);
    }
    close(fd);

    if (remain == 0) {
        /* 全部写完再回确认，客户端收到后才报告“上传完成” */
        send_i64(conn, (int64_t)got);
        printf("上传完成，本次共接收 %llu 字节\n", (unsigned long long)got);
    } else {
        printf("上传中断，已接收 %llu 字节\n", (unsigned long long)got);
    }
}

static void handle_client(int conn)
{
    struct file_request req;
    uint32_t name_len;
    uint64_t offset, total;
    char *path;

    if (recv_all(conn, &req, sizeof req) != 1) {
        printf("接收请求头失败\n");
        return;
    }
    name_len = ntohl(req.name_len);
    offset   = ntoh64(req.offset);
    total    = ntoh64(req.file_size);

    if (name_len == 0 || name_len > MAX_NAME_LEN) {
        printf("文件名长度非法：%u\n", name_len);
        send_i64(conn, -1);
        return;
    }

    path = malloc((size_t)name_len + 1);
    if (path == NULL) {
        send_i64(conn, -2);
        return;
    }
    if (recv_all(conn, path, name_len) != 1) {
        printf("接收文件名失败\n");
        free(path);
        return;
    }
    path[name_len] = '\0';

    if (req.cmd == 'D')
        handle_download(conn, path, offset);
    else if (req.cmd == 'U')
        handle_upload(conn, path, offset, total);
    else {
        printf("未知命令：0x%02x\n", req.cmd);
        send_i64(conn, -1);
    }
    free(path);
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    int listen_fd, opt = 1;
    struct sockaddr_in addr;

    if (argc >= 2)
        port = atoi(argv[1]);
    if (argc >= 3)
        throttle_ms = atoi(argv[2]);

    signal(SIGPIPE, SIG_IGN);   /* 对端断开时不让 send 杀死进程 */

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("服务器已启动，监听端口 %d", port);
    if (throttle_ms > 0)
        printf("（演示模式：每 64KB 延时 %d ms）", throttle_ms);
    printf("\n");

    for (;;) {
        struct sockaddr_in cli;
        socklen_t len = sizeof cli;
        char ip[INET_ADDRSTRLEN];
        int conn = accept(listen_fd, (struct sockaddr *)&cli, &len);

        if (conn < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);
        printf("\n客户端 %s:%d 已连接\n", ip, ntohs(cli.sin_port));

        handle_client(conn);       /* 串行服务：处理完一个再接受下一个 */
        close(conn);
        printf("客户端连接已关闭，等待下一个连接……\n");
    }

    close(listen_fd);
    return 0;
}
