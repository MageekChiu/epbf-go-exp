//go:build ignore

// #include <linux/bpf.h>
// #include <linux/ptrace.h>
// #include <linux/sched.h>
// #include <uapi/linux/ptrace.h>
// #include <uapi/linux/bpf.h>
// #include <linux/netlink.h>
#include <vmlinux.h>
// #include "common.h"
#include "bpf_endian.h"
#include "bpf_tracing.h"
#include "bpf_helpers.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
// #include <pwd.h>

#define AF_NETLINK	16

// 定义BPF map，用于用户空间和eBPF之间传递数据
// struct {
// 	__uint(type, BPF_MAP_TYPE_RINGBUF);
//     // ring buff: map create: invalid argument (without BTF k/v)
//     // https://stackoverflow.com/questions/63415220/bpf-ring-buffer-invalid-argument-22
//     // (max_entries attribute in libbpf map definition). It has to be a multiple of a memory page (which is 4096 bytes at least on most popular platforms)
// 	__uint(max_entries, 1 << 24);
// } events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 24);
} events SEC(".maps");

#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN 32
#define PATH_LEN 256
#endif

struct event_data {
    u32 pid;
    u32 uid;
	u8 comm[TASK_COMM_LEN];
	u8 path[PATH_LEN];
};

const struct event_data *unused __attribute__((unused));

#define AT_FDCWD		-100 
#define AT_REMOVEDIR		0x200
#define UNKNOW		-1

__attribute__((always_inline)) 
static int submit_event(const char *pathname, int dfd, int flag){
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    // getent passwd 1000
    // id -nu 1000
    // awk -F: -v uid=1000 '$3 == uid {print $1}' /etc/passwd
    u32 uid = bpf_get_current_uid_gid();
    // can not do this in kernel space but in user space
    // struct passwd *pw; //
    // pw = getpwuid(uid);
    // if (pw == NULL) {
    //     bpf_printk("Failed to get user\n");
    // }else{
    //     bpf_printk("Username: %s\n", pw->pw_name);
    // }

    struct event_data *data;

    data = bpf_ringbuf_reserve(&events, sizeof(struct event_data), 0);
    if (!data) {
        return 0;
    }

    if (bpf_probe_read_user(data->path, PATH_LEN, pathname) == 0) {
        bpf_printk("Data at pathname: %s\n", data->path);
    } else {
        bpf_printk("Failed to read data from pathname\n");
    }

    data->pid = pid;
    data->uid = uid;
    bpf_get_current_comm(&data->comm, TASK_COMM_LEN);
    bpf_printk("unlinkat called: pid=%d,uid=%d, dfd=%d, pathname=%d, flag=%d\n", pid, uid, dfd, pathname, flag);
    bpf_ringbuf_submit(data, 0);

    return 0;
} 

// https://elixir.bootlin.com/linux/v6.6.5/source/fs/namei.c#L4280
// cat /sys/kernel/debug/tracing/events/syscalls/sys_enter_rmdir/format
SEC("tracepoint/syscalls/sys_enter_rmdir")
int tracepoint_rmdir(struct trace_event_raw_sys_enter *ctx) {
    const char * pathname = (char *)ctx->args[0];   
    return submit_event(pathname, UNKNOW, AT_REMOVEDIR);
}

// https://elixir.bootlin.com/linux/v6.6.5/source/fs/namei.c#L4445
// cat /sys/kernel/debug/tracing/events/syscalls/sys_enter_unlink/format
SEC("tracepoint/syscalls/sys_enter_unlink")
int tracepoint_unlink(struct trace_event_raw_sys_enter *ctx) {
    const char * pathname = (char *)ctx->args[0]; 
    return submit_event(pathname, UNKNOW, UNKNOW);
}

// touch 1.txt && rm -rf 1.txt
// https://elixir.bootlin.com/linux/v6.6.5/source/fs/namei.c#L4435
// cat /sys/kernel/debug/tracing/events/syscalls/sys_enter_unlinkat/format
SEC("tracepoint/syscalls/sys_enter_unlinkat")
int tracepoint_unlinkat(struct trace_event_raw_sys_enter *ctx) {
    int dfd = ctx->args[0];   //dirfd 要相对的目录的文件描述符。    
    const char * pathname = (char *)ctx->args[1];    
    int flag = ctx->args[2];

    return submit_event(pathname, dfd, flag);
}
// struct bpf_map_def SEC("maps") optval_map = {
//     .type = BPF_MAP_TYPE_ARRAY,
//     .max_entries = 1,
//     .value_size = 2048, // 假设最大数据长度为2048字节
// };

/*

// 追踪 sendmsg 调用，过滤 netlink 消息
SEC("kprobe/netlink_sendmsg")
// https://elixir.bootlin.com/linux/v6.11.3/source/net/netlink/af_netlink.c#L1819
// The eBPF is using target specific macros, please provide -target that is not bpf, bpfel or bpfeb
// https://github.com/cilium/ebpf/discussions/772
// //go:generate go run github.com/cilium/ebpf/cmd/bpf2go -target amd64 ...
// NetlinkSendmsg: unknown program netlink_sendmsg
int BPF_KPROBE(netlink_sendmsg, struct socket *sock, struct msghdr *msg, size_t len) {
    struct event_data data = {};
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    u32 uid = bpf_get_current_uid_gid();

    // 检查是否是 netlink 消息
    if (sock->sk && sock->sk->__sk_common.skc_family == AF_NETLINK) {
        data.pid = pid;
        data.uid = uid;
        bpf_get_current_comm(&data.comm, sizeof(data.comm));

        // 向用户空间发送事件
        bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &data, sizeof(data));
    }

    return 0;
}
*/

#define SOL_IP		0
#define IPT_SO_SET_REPLACE 64 // 替换 iptables 规则的选项

// /sys/kernel/debug/tracing/events/syscalls/sys_enter_setsockopt/enable
// 捕获 tracepoint 的 setsockopt 系统调用入口
// /sys/kernel/debug/tracing/events/syscalls/sys_enter_setsockopt/format
// parameter 
SEC("tracepoint/syscalls/sys_enter_setsockopt")
int tracepoint_setsockopt(struct trace_event_raw_sys_enter *ctx) {
    int fd = ctx->args[0];       // 文件描述符
    int level = ctx->args[1];    // 协议层级
    int optname = ctx->args[2];  // 选项名
    int optlen = ctx->args[4];   // 选项长度

    // too much stack size, should use map
    // char buffer[2046]; 
    // char buffer[128]; 
    // 获取指向 map 的键
    // int key = 0;
    // char *optval = ctx->args[3] ; 
    // // char __user *optval = (char __user *)ctx->args[3]; // optval
    // if (bpf_probe_read_user(buffer, optlen, optval) == 0) {
    //     bpf_printk("Stored optval in map: %s\n", buffer);
    // } else {
    //     bpf_printk("Failed to read optval content from user space\n");
    // }

// // BPF_KPROBE supports up to five parameters, so we can't get the rest
// SEC("kprobe/__sys_setsockopt")
// int BPF_KPROBE(kprobe_sys_setsockopt, 
//     int fd, int level, int optname, char __user ){//*user_optval){//,int optlen){

// // 捕获 setsockopt() 系统调用
// SEC("kprobe/sys_setsockopt")
// int bpf_prog(struct pt_regs *ctx) {
//     int fd = PT_REGS_PARM1(ctx);       // 文件描述符
//     int level = PT_REGS_PARM2(ctx);    // 套接字协议层
//     int optname = PT_REGS_PARM3(ctx);  // 选项名
//     const void *optval = (void *)PT_REGS_PARM4(ctx); // 选项值
//     int optlen = PT_REGS_PARM5(ctx);   // 选项长度

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    u32 uid = bpf_get_current_uid_gid();

    // bpf_printk("setsockopt called: pid=%d, sockfd=%d, level=%d, optname=%d, optlen=%d"
        // , pid, fd,level, optname, optlen);
    // 只对 level == SOL_NETFILTER 的 setsockopt 进行监控
    if (level == SOL_IP && optname == IPT_SO_SET_REPLACE) {
        // 记录一些信息，比如进程 ID
        struct event_data *data;

        data = bpf_ringbuf_reserve(&events, sizeof(struct event_data), 0);
        if (!data) {
            return 0;
        }
        
        bpf_printk("ctx->args[3]: %p\n", (void *)ctx->args[3]);
        char buffer[256]; // 假设你知道将要读取的大小
        if (bpf_probe_read_user(buffer, 256, (void *)ctx->args[3]) == 0) {
            bpf_printk("Data at ctx->args[3]: %s\n", buffer);
        } else {
            bpf_printk("Failed to read data from ctx->args[3]\n");
        }

        data->pid = pid;
        data->uid = uid;
	    bpf_get_current_comm(&data->comm, TASK_COMM_LEN);
        bpf_printk("iptables setsockopt called: pid=%d, sockfd=%d, optname=%d\n", pid, fd, optname);
	    bpf_ringbuf_submit(data, 0);
    }

    return 0;
}

char __license[] SEC("license") = "Dual MIT/GPL";
// char LICENSE[] SEC("license") = "Dual BSD/GPL";

