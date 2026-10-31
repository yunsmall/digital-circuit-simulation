// 建立控制终端后再 exec shell。
//
// 内核交给 PID 1 的 fd 没有绑定控制终端（/proc/1/stat 的 tty_nr 是 -1），
// 而 busybox 的 ash 只在进程有控制终端时才启用行编辑（方向键、历史、光标移动）。
// 脚本里用重定向拿不到控制终端，getty 又碰到 dcs 未覆盖的路径，所以这个小程序
// 自己 setsid + open("/dev/console") + TIOCSCTTY，然后 exec 静态 busybox 的 sh。
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void) {
    setsid();
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return 1;
    ioctl(fd, TIOCSCTTY, 1);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
        close(fd);
    execl("/bin/sh", "sh", NULL);
    return 2;
}
