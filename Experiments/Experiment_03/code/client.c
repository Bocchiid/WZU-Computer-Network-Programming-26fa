/* client.c —— 实验3 基于流式套接字的文件传输程序（客户端）
 *
 * 编译：gcc client.c -o client -Wall
 *
 * 用法：
 *   下载：./client download <服务器IP> <端口> <服务器文件> <本地文件> [偏移]
 *   上传：./client upload   <服务器IP> <端口> <本地文件> <服务器文件> [偏移]
 *
 * 断点续传：
 *   传输过程中会把“已收/已发字节数”写进控制文件（本地文件.ctl 或 .uctl）。
 *   下次运行时如果省略偏移参数，程序自动读取控制文件，从断点继续传输；
 *   显式给偏移 0 表示放弃续传、从头开始。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BUF_SIZE      (64 * 1024)
#define MAX_NAME_LEN  4096

/* Ctrl+C 时只置位，让循环收尾后再退出，保证控制文件记录的位置是准确的 */
static volatile sig_atomic_t stop_flag = 0;

static void on_sigint(int sig)
{
    (void)sig;
    stop_flag = 1;
}

struct file_request {
    uint8_t  cmd;
    uint32_t name_len;
    uint64_t offset;
    uint64_t file_size;
} __attribute__((packed));

static uint64_t hton64(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(v & 0xffffffffu)) << 32) |
           (uint64_t)htonl((uint32_t)(v >> 32));
#else
    return v;
#endif
}
#define ntoh64 hton64

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

static int recv_i64(int fd, int64_t *value)
{
    uint64_t net;
    if (recv_all(fd, &net, sizeof net) != 1)
        return -1;
    *value = (int64_t)ntoh64(net);
    return 0;
}

/* ---------- 控制文件 ---------- */

static void ctl_name(const char *local, int upload, char *out, size_t n)
{
    snprintf(out, n, "%s.%s", local, upload ? "uctl" : "ctl");
}

static void save_ctl(const char *ctl, const char *ip, int port,
                     const char *remote, const char *local, uint64_t offset)
{
    char tmp[PATH_MAX];
    FILE *f;

    snprintf(tmp, sizeof tmp, "%s.tmp", ctl);
    f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fprintf(f, "server=%s\n", ip);
    fprintf(f, "port=%d\n", port);
    fprintf(f, "remote=%s\n", remote);
    fprintf(f, "local=%s\n", local);
    fprintf(f, "offset=%llu\n", (unsigned long long)offset);
    fclose(f);
    rename(tmp, ctl);          /* 先写临时文件再改名，避免文件写坏 */
}

static int load_ctl(const char *ctl, char *ip, size_t ip_n, int *port,
                    char *remote, size_t remote_n, char *local, size_t local_n,
                    uint64_t *offset)
{
    char line[1024];
    FILE *f = fopen(ctl, "r");

    if (f == NULL)
        return -1;

    while (fgets(line, sizeof line, f) != NULL) {
        char *eq = strchr(line, '=');
        char *val;

        if (eq == NULL)
            continue;
        *eq = '\0';
        val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';

        if (strcmp(line, "server") == 0)
            snprintf(ip, ip_n, "%s", val);
        else if (strcmp(line, "port") == 0)
            *port = atoi(val);
        else if (strcmp(line, "remote") == 0)
            snprintf(remote, remote_n, "%s", val);
        else if (strcmp(line, "local") == 0)
            snprintf(local, local_n, "%s", val);
        else if (strcmp(line, "offset") == 0)
            *offset = strtoull(val, NULL, 10);
    }
    fclose(f);
    return 0;
}

/* ---------- 连接与请求 ---------- */

static int connect_server(const char *ip, int port)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "服务器 IP 地址不合法：%s\n", ip);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(int fd, uint8_t cmd, const char *name,
                        uint64_t offset, uint64_t file_size)
{
    struct file_request req;
    size_t len = strlen(name);

    if (len == 0 || len > MAX_NAME_LEN) {
        fprintf(stderr, "文件名长度不合法\n");
        return -1;
    }
    req.cmd       = cmd;
    req.name_len  = htonl((uint32_t)len);
    req.offset    = hton64(offset);
    req.file_size = hton64(file_size);

    if (send_all(fd, &req, sizeof req) != 1)
        return -1;
    if (send_all(fd, name, len) != 1)
        return -1;
    return 0;
}

static void print_error(int64_t code)
{
    switch (code) {
    case -1: fprintf(stderr, "服务器无法访问该文件（不存在或没有权限）\n"); break;
    case -2: fprintf(stderr, "服务器读取文件信息失败\n"); break;
    case -3: fprintf(stderr, "起始偏移量超过文件长度\n"); break;
    default: fprintf(stderr, "服务器返回错误码：%lld\n", (long long)code); break;
    }
}

/* ---------- 下载 ---------- */
static int do_download(int sock, const char *ip, int port,
                       const char *remote, const char *local,
                       uint64_t offset, int resume, const char *ctl)
{
    char buf[BUF_SIZE];
    int64_t total;
    uint64_t remain, received = 0;
    int out, flags;
    struct stat st;

    /* 只有“读控制文件自动续传”才要求本地文件已经存在；
     * 命令行显式给偏移时，允许新建本地文件，直接从该偏移开始接收。 */
    if (resume && offset > 0) {
        if (stat(local, &st) != 0 || (uint64_t)st.st_size < offset) {
            fprintf(stderr, "本地文件不存在或长度不足 %llu 字节，改为从头下载\n",
                    (unsigned long long)offset);
            offset = 0;
        }
    }

    if (send_request(sock, 'D', remote, offset, 0) != 0) {
        fprintf(stderr, "发送请求失败\n");
        return 1;
    }
    if (recv_i64(sock, &total) != 0) {
        fprintf(stderr, "接收服务器响应失败\n");
        return 1;
    }
    if (total < 0) {
        print_error(total);
        return 1;
    }
    printf("服务器文件长度：%lld 字节，从偏移 %llu 开始接收\n",
           (long long)total, (unsigned long long)offset);

    if ((uint64_t)total < offset) {
        fprintf(stderr, "偏移超过文件长度\n");
        return 1;
    }

    flags = O_WRONLY | O_CREAT;
    if (offset == 0)
        flags |= O_TRUNC;
    out = open(local, flags, 0644);
    if (out < 0) {
        perror("open local file");
        return 1;
    }
    if (offset > 0 && lseek(out, (off_t)offset, SEEK_SET) < 0) {
        perror("lseek");
        close(out);
        return 1;
    }

    save_ctl(ctl, ip, port, remote, local, offset);
    remain = (uint64_t)total - offset;

    while (remain > 0) {
        size_t want = remain > BUF_SIZE ? BUF_SIZE : (size_t)remain;
        ssize_t r;

        if (stop_flag) {
            printf("\n收到中断信号，停止接收\n");
            break;
        }
        r = recv(sock, buf, want, 0);
        if (r == 0) {
            printf("\n服务器关闭了连接\n");
            break;
        }
        if (r < 0) {
            if (errno == EINTR) {
                if (stop_flag) {
                    printf("\n收到中断信号，停止接收\n");
                    break;
                }
                continue;
            }
            perror("recv");
            break;
        }
        if (write_all(out, buf, (size_t)r) < 0) {
            perror("write");
            break;
        }
        received += (uint64_t)r;
        remain -= (uint64_t)r;

        /* 每收一块就更新控制文件，Ctrl+C 后也能从准确位置续传 */
        save_ctl(ctl, ip, port, remote, local, offset + received);
        /* 每跨过 1MB 汇报一次进度，避免刷屏 */
        if ((offset + received) / (1024 * 1024) !=
            (offset + received - (uint64_t)r) / (1024 * 1024) || remain == 0) {
            printf("\r已接收 %llu / %lld 字节（%.1f%%）",
                   (unsigned long long)(offset + received), (long long)total,
                   total ? (double)(offset + received) * 100.0 / (double)total : 100.0);
            fflush(stdout);
        }
    }
    close(out);
    printf("\n");

    if (remain == 0) {
        printf("下载完成：%s（本次接收 %llu 字节，文件总长度 %lld 字节）\n",
               local, (unsigned long long)received, (long long)total);
        unlink(ctl);            /* 完成后删除控制文件 */
        return 0;
    }
    printf("传输中断，已保存 %llu 字节。\n", (unsigned long long)(offset + received));
    printf("重新执行同一条命令即可从断点继续（控制文件：%s）\n", ctl);
    return 1;
}

/* ---------- 上传 ---------- */
static int do_upload(int sock, const char *ip, int port,
                     const char *local, const char *remote,
                     uint64_t offset, const char *ctl)
{
    char buf[BUF_SIZE];
    struct stat st;
    int64_t resp;
    uint64_t total, remain, sent = 0;
    int in;

    in = open(local, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "打开本地文件 %s 失败：%s\n", local, strerror(errno));
        return 1;
    }
    if (fstat(in, &st) < 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "本地文件 %s 不是普通文件\n", local);
        close(in);
        return 1;
    }
    total = (uint64_t)st.st_size;
    if (offset > total) {
        fprintf(stderr, "偏移 %llu 超过本地文件长度 %llu\n",
                (unsigned long long)offset, (unsigned long long)total);
        close(in);
        return 1;
    }
    if (lseek(in, (off_t)offset, SEEK_SET) < 0) {
        perror("lseek");
        close(in);
        return 1;
    }

    if (send_request(sock, 'U', remote, offset, total) != 0) {
        fprintf(stderr, "发送请求失败\n");
        close(in);
        return 1;
    }
    if (recv_i64(sock, &resp) != 0) {
        fprintf(stderr, "接收服务器响应失败\n");
        close(in);
        return 1;
    }
    if (resp < 0) {
        print_error(resp);
        close(in);
        return 1;
    }
    printf("上传 %s -> %s，文件长度 %llu 字节，从偏移 %llu 开始发送\n",
           local, remote, (unsigned long long)total,
           (unsigned long long)offset);

    save_ctl(ctl, ip, port, remote, local, offset);
    remain = total - offset;

    while (remain > 0) {
        size_t want = remain > BUF_SIZE ? BUF_SIZE : (size_t)remain;
        ssize_t r;

        if (stop_flag) {
            printf("\n收到中断信号，停止发送\n");
            break;
        }
        r = read(in, buf, want);
        if (r < 0) {
            if (errno == EINTR) {
                if (stop_flag) {
                    printf("\n收到中断信号，停止发送\n");
                    break;
                }
                continue;
            }
            perror("read");
            break;
        }
        if (r == 0)
            break;
        if (send_all(sock, buf, (size_t)r) != 1) {
            printf("\n服务器关闭了连接\n");
            break;
        }
        sent += (uint64_t)r;
        remain -= (uint64_t)r;

        save_ctl(ctl, ip, port, remote, local, offset + sent);
        if ((offset + sent) / (1024 * 1024) !=
            (offset + sent - (uint64_t)r) / (1024 * 1024) || remain == 0) {
            printf("\r已发送 %llu / %llu 字节（%.1f%%）",
                   (unsigned long long)(offset + sent), (unsigned long long)total,
                   total ? (double)(offset + sent) * 100.0 / (double)total : 100.0);
            fflush(stdout);
        }
    }
    close(in);
    printf("\n");

    if (remain == 0) {
        int64_t ack;

        /* 等服务器确认全部写入磁盘，避免“客户端说完成、服务器还没写完” */
        if (recv_i64(sock, &ack) != 0 ||
            ack != (int64_t)(total - offset)) {
            fprintf(stderr, "服务器未确认接收完整文件\n");
            return 1;
        }
        printf("上传完成：%s（共 %llu 字节）\n", remote,
               (unsigned long long)total);
        unlink(ctl);
        return 0;
    }
    printf("传输中断，已发送 %llu 字节。\n", (unsigned long long)(offset + sent));
    printf("重新执行同一条命令即可从断点继续（控制文件：%s）\n", ctl);
    return 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法：\n"
            "  %s download <服务器IP> <端口> <服务器文件> <本地文件> [偏移]\n"
            "  %s upload   <服务器IP> <端口> <本地文件> <服务器文件> [偏移]\n"
            "省略偏移时，若存在控制文件则自动断点续传；写 0 表示从头开始。\n",
            prog, prog);
}

int main(int argc, char *argv[])
{
    const char *mode, *ip, *src, *dst, *local, *remote;
    char ctl[PATH_MAX], saved_ip[64] = "", saved_remote[PATH_MAX] = "";
    char saved_local[PATH_MAX] = "";
    int upload, port, sock, has_offset, resume = 0;
    int saved_port = 0;
    uint64_t offset = 0, saved_offset = 0;
    int ret;

    if (argc < 6) {
        usage(argv[0]);
        return 1;
    }

    mode      = argv[1];
    ip        = argv[2];
    port      = atoi(argv[3]);
    src       = argv[4];
    dst       = argv[5];
    has_offset = (argc >= 7);

    if (strcmp(mode, "download") == 0) {
        upload = 0;
        remote = src;
        local  = dst;
    } else if (strcmp(mode, "upload") == 0) {
        upload = 1;
        local  = src;
        remote = dst;
    } else {
        usage(argv[0]);
        return 1;
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "端口不合法：%s\n", argv[3]);
        return 1;
    }

    ctl_name(local, upload, ctl, sizeof ctl);

    if (has_offset) {
        offset = strtoull(argv[6], NULL, 10);
    } else if (load_ctl(ctl, saved_ip, sizeof saved_ip, &saved_port,
                        saved_remote, sizeof saved_remote,
                        saved_local, sizeof saved_local, &saved_offset) == 0) {
        /* 只有控制文件确实属于这次任务时才自动续传 */
        if (strcmp(saved_ip, ip) == 0 && saved_port == port &&
            strcmp(saved_remote, remote) == 0 &&
            strcmp(saved_local, local) == 0) {
            offset = saved_offset;
            resume = 1;
            printf("检测到控制文件 %s，从 %llu 字节处续传\n",
                   ctl, (unsigned long long)offset);
        } else {
            printf("控制文件与本次任务不匹配，从头开始\n");
        }
    }

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sigint;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;       /* 不设 SA_RESTART，让阻塞的 recv 及时返回 EINTR */
        sigaction(SIGINT, &sa, NULL);
    }

    printf("正在连接 %s:%d ……\n", ip, port);
    sock = connect_server(ip, port);
    if (sock < 0)
        return 1;
    printf("连接成功\n");

    if (upload)
        ret = do_upload(sock, ip, port, local, remote, offset, ctl);
    else
        ret = do_download(sock, ip, port, remote, local, offset, resume, ctl);

    close(sock);
    return ret;
}
