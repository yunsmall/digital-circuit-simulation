#include "SlirpBackend.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <thread>
#include <vector>

#include <libslirp.h>

#include "dcs/components/platform/VirtioNet.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dsc::net {

namespace {

struct Timer {
    // timer_mod 的 expire_time 是毫秒：libslirp 先把 clock_get_ns() 除以
    // SCALE_MS 再传进来（见 slirp.c/ip6_icmp.c），不能拿纳秒比较
    int64_t expire_ms = 0;
    void (*cb)(void *) = nullptr;
    void *opaque = nullptr;
    bool fired = false;
    // 每个定时器一个唯一代次：到期列表里存指针 + 代次，回调里释放并重新
    // 分配后即使地址复用，也不会把新定时器当成旧的提前触发
    uint64_t gen = 0;
};

struct SlirpBackend {
    Slirp *slirp = nullptr;
    VirtioNet *net = nullptr;
    std::vector<Timer *> timers; // 只在后台线程访问
    std::thread worker;
    std::atomic<bool> stop{false};
    // 唤醒通道：JIT 每推入一个 TX 包写一个字节，后端线程阻塞在它和网络 fd 上
    slirp_os_socket kick_rd = SLIRP_INVALID_SOCKET;
    slirp_os_socket kick_wr = SLIRP_INVALID_SOCKET;
    // WSAStartup 会做进程级引用计数，成功一次就要配对一次 WSACleanup
    bool wsa_started = false;
    // 定时器代次发号器，见 Timer::gen
    uint64_t next_gen = 1;

    void run();
};

// timer_mod 给的是毫秒（libslirp 内部把 clock_get_ns() / SCALE_MS 后再设）
int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
short toPollEvents(int ev) {
    short r = 0;
    if (ev & SLIRP_POLL_IN) r |= POLLRDNORM;
    if (ev & SLIRP_POLL_OUT) r |= POLLWRNORM;
    return r;
}
int fromPollEvents(short re) {
    int r = 0;
    if (re & (POLLRDNORM | POLLIN)) r |= SLIRP_POLL_IN;
    if (re & (POLLWRNORM | POLLOUT)) r |= SLIRP_POLL_OUT;
    if (re & POLLERR) r |= SLIRP_POLL_ERR;
    if (re & POLLHUP) r |= SLIRP_POLL_HUP;
    return r;
}
#else
short toPollEvents(int ev) {
    short r = 0;
    if (ev & SLIRP_POLL_IN) r |= POLLIN;
    if (ev & SLIRP_POLL_OUT) r |= POLLOUT;
    return r;
}
int fromPollEvents(short re) {
    int r = 0;
    if (re & POLLIN) r |= SLIRP_POLL_IN;
    if (re & POLLOUT) r |= SLIRP_POLL_OUT;
    if (re & POLLERR) r |= SLIRP_POLL_ERR;
    if (re & POLLHUP) r |= SLIRP_POLL_HUP;
    return r;
}
#endif

struct PollEntry {
    slirp_os_socket fd;
    short events;
    short revents;
};

// slirp_pollfds_fill_socket 的 add_poll 回调上下文。回调返回的索引会被
// libslirp 原样存下来、稍后交给 get_revents，返回 -1 就是越界索引；
// 分配失败只能标记后放弃本轮
struct PollFill {
    std::vector<PollEntry> *entries = nullptr;
    bool failed = false;
};

// ---- 唤醒通道 ----

// 唤醒报文丢了没关系（poll 最多 1 秒自己醒来），所以发送端必须非阻塞：
// 阻塞 send 在缓冲区满时会把 JIT 线程或销毁路径卡死
void setSendNonBlocking(slirp_os_socket s) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Windows 没有 socketpair，用一对连到彼此的 UDP 回环 socket 代替
bool makeWakePair(slirp_os_socket &rd, slirp_os_socket &wr) {
#ifdef _WIN32
    auto bindAny = [](SOCKET s, sockaddr_in &out) -> bool {
        out = {};
        out.sin_family = AF_INET;
        out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        out.sin_port = 0;
        if (bind(s, (sockaddr *) &out, sizeof(out)) != 0)
            return false;
        int len = sizeof(out);
        return getsockname(s, (sockaddr *) &out, &len) == 0;
    };
    SOCKET a = socket(AF_INET, SOCK_DGRAM, 0);
    SOCKET b = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in aa{}, bb{};
    if (a == INVALID_SOCKET || b == INVALID_SOCKET || !bindAny(a, aa) || !bindAny(b, bb)) {
        if (a != INVALID_SOCKET) closesocket(a);
        if (b != INVALID_SOCKET) closesocket(b);
        return false;
    }
    if (connect(a, (sockaddr *) &bb, sizeof(bb)) != 0 || connect(b, (sockaddr *) &aa, sizeof(aa)) != 0) {
        closesocket(a);
        closesocket(b);
        return false;
    }
    wr = a;
    rd = b;
    setSendNonBlocking(wr);
    return true;
#else
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0)
        return false;
    wr = fds[0];
    rd = fds[1];
    setSendNonBlocking(wr);
    return true;
#endif
}

void closeWakePair(SlirpBackend *be) {
#ifdef _WIN32
    if (be->kick_rd != INVALID_SOCKET) closesocket(be->kick_rd);
    if (be->kick_wr != INVALID_SOCKET) closesocket(be->kick_wr);
#else
    if (be->kick_rd >= 0) ::close(be->kick_rd);
    if (be->kick_wr >= 0) ::close(be->kick_wr);
#endif
    be->kick_rd = be->kick_wr = SLIRP_INVALID_SOCKET;
}

// 和 backendCreate 里的 WSAStartup 配对：引用计数成功加过才减，
// 失败路径同样要调用（在 socket 都关掉之后）
void cleanupWinsock(SlirpBackend *be) {
#ifdef _WIN32
    if (be->wsa_started) {
        WSACleanup();
        be->wsa_started = false;
    }
#else
    (void) be;
#endif
}

void wakeBackend(SlirpBackend *be) {
    if (be->kick_wr == SLIRP_INVALID_SOCKET)
        return;
    const char one = 1;
    // 发送端非阻塞：缓冲区满或对端已停时失败都无所谓，run() 的 poll 有
    // 1 秒上限会自己醒来。kick 与 destroy 的并发由 VirtioNet 保证不存在
    // （deinit 之后不会再有 tick）
    send(be->kick_wr, &one, 1, 0);
}

void drainWake(SlirpBackend *be) {
    // socket 是阻塞的：循环 recv 第二次就会永远卡住，整个后端线程再也不推进。
    // 每次 kick 恰好发一个 1 字节数据报，读一次就清干净了
    char buf[64];
    recv(be->kick_rd, buf, sizeof(buf), 0);
}

// ---- slirp 回调（全部在后台线程里调用）----

slirp_ssize_t cbSendPacket(const void *buf, size_t len, void *opaque) {
    auto *be = static_cast<SlirpBackend *>(opaque);
    // 环满就丢，TCP 会重传；这里是后端线程直接写设备的 RX 环。
    // pushRxPacket 内部要拿 mutex，锁失败会抛 system_error，不能让它穿出
    // C 回调边界
    try {
        be->net->pushRxPacket((const uint8_t *) buf, (uint32_t) len);
    } catch (...) {
        return 0;
    }
    return (slirp_ssize_t) len;
}

void cbGuestError(const char *msg, void * /*opaque*/) {
    std::fprintf(stderr, "[slirp] %s\n", msg);
}

// slirp 在 <6 的配置下会退回调用 register_poll_fd，而它对我们没有意义；
// 用版本 6 的接口并实现成空操作。后端线程每轮都用 slirp_pollfds_fill_socket
// 重建监听集合，所有回调也都在同一线程里执行，不需要额外唤醒
void cbRegisterPollSocket(slirp_os_socket /*fd*/, void * /*opaque*/) {}
void cbUnregisterPollSocket(slirp_os_socket /*fd*/, void * /*opaque*/) {}

// 缓冲区腾空后的唤醒；同一线程里处理完就轮到 poll，轮不到异步通知
void cbNotify(void * /*opaque*/) {}

int64_t cbClockGetNs(void * /*opaque*/) {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void *cbTimerNew(SlirpTimerCb cb, void *cb_opaque, void *opaque) {
    auto *be = static_cast<SlirpBackend *>(opaque);
    // 这是 C 回调，异常穿过 slirp 的 C 栈帧是未定义行为：分配失败时返回空，
    // 由 cbTimerMod/cbTimerFree 容忍（最坏情况是网络超时定时器不工作）
    auto *t = new (std::nothrow) Timer;
    if (!t)
        return nullptr;
    t->cb = cb;
    t->opaque = cb_opaque;
    t->expire_ms = INT64_MAX;
    t->gen = be->next_gen++;
    try {
        be->timers.push_back(t);
    } catch (...) {
        delete t;
        return nullptr;
    }
    return t;
}

void cbTimerFree(void *timer, void *opaque) {
    if (!timer)
        return;
    auto *be = static_cast<SlirpBackend *>(opaque);
    auto *t = static_cast<Timer *>(timer);
    auto &v = be->timers;
    v.erase(std::remove(v.begin(), v.end(), t), v.end());
    delete t;
}

void cbTimerMod(void *timer, int64_t expire_time, void * /*opaque*/) {
    if (!timer)
        return;
    auto *t = static_cast<Timer *>(timer);
    t->expire_ms = expire_time;
    t->fired = false;
}

// slirp_cleanup 正常会通过 timer_free 回调清空列表；失败路径没有这个保证，
// 销毁前统一兜底，避免 Timer 泄漏
void freeTimers(SlirpBackend *be) {
    for (auto *t: be->timers)
        delete t;
    be->timers.clear();
}

// ---- 后台事件循环 ----

void SlirpBackend::run() {
    // 线程入口一旦让异常逃出去就是 std::terminate（整个仿真进程直接没了），
    // 这里的临时 vector 扩容、分配都可能抛 bad_alloc，必须在这里兜住
    try {
        uint8_t pkt[1600];
        while (!stop.load(std::memory_order_relaxed)) {
            // guest → slirp：设备 TX 环由 JIT 填写，这里原子取走
            uint32_t n;
            while ((n = net->popTxPacket(pkt, sizeof(pkt))) != 0)
                slirp_input(slirp, pkt, (int) n);

            const int64_t now = nowMs();
            // 回调里可能新建或释放定时器（slirp 的 TCP 定时器会这么做），
            // 直接遍历 vector 会在回调中失效，先收集到期项再逐个调用
            std::vector<Timer *> due;
            std::vector<uint64_t> due_gen;
            for (auto *t: timers) {
                if (!t->fired && t->expire_ms <= now) {
                    t->fired = true;
                    due.push_back(t);
                    due_gen.push_back(t->gen);
                }
            }
            for (size_t i = 0; i < due.size(); i++) {
                Timer *t = due[i];
                // 前面的回调可能已经把后面的定时器释放掉；地址还可能被
                // 新分配的 Timer 复用，所以指针和代次都要对得上。
                // 回调里也可能用 timer_mod 把它推到未来（fired 已清），
                // 这一轮就不该再执行
                auto it = std::find(timers.begin(), timers.end(), t);
                if (it == timers.end() || (*it)->gen != due_gen[i] || !t->fired)
                    continue;
                auto cb = t->cb;
                void *opaque = t->opaque;
                // 这是 C 回调，异常穿过 slirp 的 C 栈帧是未定义行为；
                // 单个定时器出错不能把后台线程踢出循环（join 会等不到线程退出）
                try {
                    cb(opaque);
                } catch (...) {
                }
            }

            std::vector<PollEntry> entries;
            uint32_t timeout = UINT32_MAX;
            PollFill fill{&entries};
            slirp_pollfds_fill_socket(
                    slirp, &timeout,
                    [](slirp_os_socket fd, int events, void *opaque) -> int {
                        // 这是传给 libslirp 的 C 回调，异常穿过 C 栈帧是未定义行为。
                        // 分配失败时不能返回 -1（会被当成索引），标记后返回 0
                        auto *f = static_cast<PollFill *>(opaque);
                        try {
                            const int idx = (int) f->entries->size();
                            f->entries->push_back({fd, toPollEvents(events), 0});
                            return idx;
                        } catch (...) {
                            f->failed = true;
                            return 0;
                        }
                    },
                    &fill);
            if (fill.failed) {
                // fd 集合不完整，pollfds_poll 的索引对不上：本轮只等唤醒
                // 通道，下一轮重新收集。OOM 是极端情况，退化成网络暂停
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // 唤醒通道排在最后；它的 revents 不参与 slirp 的 get_revents
            const int kick_idx = (int) entries.size();
            entries.push_back({kick_rd, toPollEvents(SLIRP_POLL_IN), 0});

            // 上限 1 秒：唤醒报文丢了（send 失败/对端缓冲满）或 stop 标志刚置位时，
            // 线程也能自己醒来检查 stop，销毁时的 join 不会被无限期的 poll 卡死
            const uint32_t timeout_ms = std::min<uint32_t>(timeout, 1000);
            const int wait_ms = (int) timeout_ms;

            int rc = 0;
#ifdef _WIN32
            std::vector<WSAPOLLFD> fds(entries.size());
            for (size_t i = 0; i < entries.size(); i++) {
                fds[i].fd = entries[i].fd;
                fds[i].events = entries[i].events;
                fds[i].revents = 0;
            }
            rc = WSAPoll(fds.data(), (ULONG) fds.size(), wait_ms);
            for (size_t i = 0; i < entries.size(); i++)
                entries[i].revents = fds[i].revents;
#else
            std::vector<pollfd> fds(entries.size());
            for (size_t i = 0; i < entries.size(); i++) {
                fds[i].fd = entries[i].fd;
                fds[i].events = entries[i].events;
                fds[i].revents = 0;
            }
            rc = ::poll(fds.data(), fds.size(), wait_ms);
            for (size_t i = 0; i < entries.size(); i++)
                entries[i].revents = fds[i].revents;
#endif

            // poll 失败（rc<0）时不读 revents：POSIX 没保证失败时会把它们清零
            if (rc > 0 && entries[kick_idx].revents != 0)
                drainWake(this);

            slirp_pollfds_poll(
                    slirp, rc < 0,
                    [](int idx, void *opaque) -> int {
                        auto *v = static_cast<std::vector<PollEntry> *>(opaque);
                        return fromPollEvents((*v)[idx].revents);
                    },
                    &entries);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[slirp] 后端线程异常退出: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "[slirp] 后端线程未知异常退出\n");
    }
}

// ---- BackendOps 实现 ----

// slirp_new 只保存这个表的指针（slirp.c: "slirp->cb = callbacks"），
// 所以它必须有静态存储期，不能放在 backendCreate 的栈上
const SlirpCb &slirpCallbacks() {
    static const SlirpCb cb = [] {
        SlirpCb c{};
        c.send_packet = &cbSendPacket;
        c.guest_error = &cbGuestError;
        c.clock_get_ns = &cbClockGetNs;
        c.timer_new = &cbTimerNew;
        c.timer_free = &cbTimerFree;
        c.timer_mod = &cbTimerMod;
        c.notify = &cbNotify;
        c.register_poll_socket = &cbRegisterPollSocket;
        c.unregister_poll_socket = &cbUnregisterPollSocket;
        return c;
    }();
    return cb;
}

// 主机端口转发（slirp 专有选项 backend_opts.hostfwd）：
//   hostfwd = [tcp|udp:]宿主地址:宿主端口-客机地址:客机端口[;多条]
// 宿主地址省略时监听 127.0.0.1，客机地址省略时是 10.0.2.15（和 qemu 的写法一致）
bool parseHostPort(const std::string &s, in_addr &addr, int &port, const char *def_addr) {
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos)
        return false;
    const std::string host = colon == 0 ? def_addr : s.substr(0, colon);
    const std::string port_s = s.substr(colon + 1);
    if (port_s.empty() || port_s.size() > 5)
        return false;
    // atoi 会接受 "12abc" 这种前缀数字、把后面的垃圾静默丢掉；
    // 端口必须是完整数字串
    int value = 0;
    const auto [end, ec] = std::from_chars(port_s.data(), port_s.data() + port_s.size(), value);
    if (ec != std::errc{} || end != port_s.data() + port_s.size())
        return false;
    if (value <= 0 || value > 65535)
        return false;
    port = value;
    return inet_pton(AF_INET, host.c_str(), &addr) == 1;
}

void applyHostForwards(Slirp *slirp, const std::string &spec) {
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t semi = spec.find(';', pos);
        std::string item = spec.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? spec.size() : semi + 1;

        bool udp = false;
        if (item.rfind("udp:", 0) == 0) {
            udp = true;
            item = item.substr(4);
        } else if (item.rfind("tcp:", 0) == 0) {
            item = item.substr(4);
        }
        const size_t dash = item.find('-');
        if (dash == std::string::npos) {
            std::fprintf(stderr, "[slirp] hostfwd 格式应为 宿主地址:端口-客机地址:端口：%s\n", item.c_str());
            continue;
        }
        in_addr host_addr{}, guest_addr{};
        int host_port = 0, guest_port = 0;
        if (!parseHostPort(item.substr(0, dash), host_addr, host_port, "127.0.0.1") ||
            !parseHostPort(item.substr(dash + 1), guest_addr, guest_port, "10.0.2.15")) {
            std::fprintf(stderr, "[slirp] hostfwd 解析失败：%s\n", item.c_str());
            continue;
        }
        if (slirp_add_hostfwd(slirp, udp ? 1 : 0, host_addr, host_port, guest_addr, guest_port) != 0) {
            std::fprintf(stderr, "[slirp] hostfwd 注册失败（端口被占用？）：%s\n", item.c_str());
            continue;
        }
        std::fprintf(stderr, "[slirp] 端口转发 %s:%d -> 客机 %d\n", udp ? "udp" : "tcp", host_port, guest_port);
    }
}

void *backendCreate(void *cfg) {
    // 先分配对象再 WSAStartup：分配失败时还没有拿走引用计数，不需要回滚
    auto *be = new SlirpBackend;
    be->net = static_cast<VirtioNet *>(cfg);
#ifdef _WIN32
    WSADATA wsa;
    be->wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif

    if (!makeWakePair(be->kick_rd, be->kick_wr)) {
        std::fprintf(stderr, "[slirp] 无法创建唤醒 socket，网络后端不可用\n");
        cleanupWinsock(be);
        delete be;
        return nullptr;
    }

    SlirpConfig sc{};
    // 版本 6 走 register_poll_socket 接口；版本 4 会退回调用已废弃的
    // register_poll_fd，那个回调是空的，ICMP 转发到外网时会跳到 0 地址
    sc.version = 6;
    sc.restricted = 0;
    sc.in_enabled = true;
    sc.vnetwork.s_addr = htonl(0x0A000200u);    // 10.0.2.0
    sc.vnetmask.s_addr = htonl(0xFFFFFF00u);    // 255.255.255.0
    sc.vhost.s_addr = htonl(0x0A000202u);       // 10.0.2.2
    sc.vdhcp_start.s_addr = htonl(0x0A00020Fu); // 10.0.2.15
    sc.vnameserver.s_addr = htonl(0x0A000203u); // 10.0.2.3
    sc.vhostname = "dcs";
    sc.if_mtu = 1500;
    sc.if_mru = 1500;
    sc.in6_enabled = false;

    be->slirp = slirp_new(&sc, &slirpCallbacks(), be);
    if (!be->slirp) {
        std::fprintf(stderr, "[slirp] slirp_new 失败，网络后端不可用\n");
        closeWakePair(be);
        cleanupWinsock(be);
        // 失败路径没有 slirp_cleanup 可调，自己回收可能已注册的定时器
        freeTimers(be);
        delete be;
        return nullptr;
    }
    // 后端私有选项：主机端口转发（元件不解释这些键，见 dcs/net/BackendOpts.h）
    // 解析里的字符串构造可能抛 bad_alloc，不能穿出 create 的“失败返回 nullptr”契约
    try {
        if (be->net) {
            const auto &opts = be->net->backendOpts();
            if (auto it = opts.find("hostfwd"); it != opts.end())
                applyHostForwards(be->slirp, it->second);
        }
    } catch (...) {
        std::fprintf(stderr, "[slirp] hostfwd 解析失败，网络后端不可用\n");
        slirp_cleanup(be->slirp);
        freeTimers(be);
        closeWakePair(be);
        cleanupWinsock(be);
        delete be;
        return nullptr;
    }
    // 线程创建可能因系统资源不足抛 system_error；这时 be/slirp/socket 都已分配，
    // 异常一旦穿到 Circuit::init，设备侧拿不到 backend_ctx，destroy 不会执行
    try {
        be->worker = std::thread(&SlirpBackend::run, be);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[slirp] 无法创建后端线程，网络后端不可用: %s\n", e.what());
        slirp_cleanup(be->slirp);
        freeTimers(be);
        closeWakePair(be);
        cleanupWinsock(be);
        delete be;
        return nullptr;
    }
    return be;
}

void backendDestroy(void *ctx) {
    auto *be = static_cast<SlirpBackend *>(ctx);
    be->stop.store(true, std::memory_order_relaxed);
    wakeBackend(be); // 让阻塞中的 poll 立刻返回
    if (be->worker.joinable())
        be->worker.join();
    if (be->slirp)
        slirp_cleanup(be->slirp);
    closeWakePair(be);
    freeTimers(be);
    cleanupWinsock(be);
    delete be;
}

void backendKick(void *ctx) {
    wakeBackend(static_cast<SlirpBackend *>(ctx));
}

void backendReset(void * /*ctx*/) {
    // 设备的队列状态由 JIT 自己清零；slirp 的连接状态留着，
    // 驱动重新初始化后会重新发 DHCP/ARP
}

} // namespace

VirtioNet::BackendOps slirpBackendOps() {
    VirtioNet::BackendOps ops;
    ops.create = &backendCreate;
    ops.destroy = &backendDestroy;
    ops.kick = &backendKick;
    ops.reset = &backendReset;
    return ops;
}

} // namespace dsc::net

// 后端自注册：dcs_soc 链接到这个文件就会生效
DCS_REGISTER_VIRTIO_NET_BACKEND("slirp", ::dsc::net::slirpBackendOps());
