#!/bin/bash
# 用 clang 交叉编译 riscv64 静态 busybox，并打成 initramfs。
# 整套流程只依赖已有的 clang-22/lld，不需要安装 GCC 交叉工具链（不需要 sudo）。
#
# 三个关键点：
#   1. musl 只装静态部分（libc.a + crt*.o + 头文件）；共享库 libc.so 需要
#      compiler-rt 的 soft-float 符号，Ubuntu 的 clang 包不带 riscv64 版本，跳过即可
#   2. 内核 UAPI 头用 `make headers_install` 生成，不要从宿主 x86 的 /usr/include 里凑
#   3. 链接时缺的 __floatsitf/__multf3 等由 Debian 的 riscv64 libgcc.a 提供
#      （只 dpkg -x 解压出这个 .a，不安装编译器）
set -e
# 必须在任何 cd 之前算好：后面会切到 busybox 源码目录，$0 是相对路径时再算就失效了
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# 有些发行版只提供带版本后缀的工具名，按"通用名优先、版本后缀兜底"探测
pick_tool() {
    for c in "$@"; do
        if command -v "$c" > /dev/null 2>&1; then
            echo "$c"
            return 0
        fi
    done
    echo "缺少工具: $*" >&2
    return 1
}
CC_BIN=${CC_BIN:-$(pick_tool clang clang-23 clang-22 clang-21 clang-20 clang-19)}
AR_BIN=${AR_BIN:-$(pick_tool llvm-ar llvm-ar-23 llvm-ar-22 llvm-ar-21 llvm-ar-20)}
RANLIB_BIN=${RANLIB_BIN:-$(pick_tool llvm-ranlib llvm-ranlib-23 llvm-ranlib-22 llvm-ranlib-21 llvm-ranlib-20)}
STRIP_BIN=${STRIP_BIN:-$(pick_tool llvm-strip llvm-strip-23 llvm-strip-22 llvm-strip-21 llvm-strip-20)}

SRC=${LINUX_SRC:-$HOME/code/linux/riscv64}
SYSROOT=/tmp/musl-rv
BB=$SRC/src/busybox-1.36.1
TARGET="riscv64-linux-gnu"
# Debian 交叉包的 GCC 安装目录。clang 的 --gcc-install-dir 直接从这里取
# crtbegin*.o / libgcc.a / libgcc_eh.a（--gcc-toolchain 认的是另一种布局，
# 对 Debian 的 gcc-cross 目录不生效）。装了下面这个包就有：
#   sudo apt install libgcc-14-dev-riscv64-cross
GCC_INSTALL=$(ls -d /usr/lib/gcc-cross/riscv64-linux-gnu/*/ 2>/dev/null | head -1)
GCC_INSTALL=${GCC_INSTALL%/}
CC="$CC_BIN --target=$TARGET -march=rv64gc -mabi=lp64d --sysroot=$SYSROOT -fuse-ld=lld"
if [ -n "$GCC_INSTALL" ]; then
    CC="$CC --gcc-install-dir=$GCC_INSTALL"
fi

# ---- 1. musl 静态库 ----
if [ ! -f $SYSROOT/lib/libc.a ]; then
    cd $SRC/src && tar xzf musl-1.2.5.tar.gz
    cd musl-1.2.5
    CC="$CC" AR=$AR_BIN RANLIB=$RANLIB_BIN \
        ./configure --prefix=$SYSROOT --target=$TARGET > /dev/null
    make -j6 > /dev/null 2>&1 || true   # libc.so 链接会失败，静态库已经在 lib/ 里
    mkdir -p $SYSROOT/include $SYSROOT/lib
    cp -rf include/. $SYSROOT/include/
    cp -rf obj/include/. $SYSROOT/include/
    cp -rf arch/generic/bits/. $SYSROOT/include/bits/ 2>/dev/null || true
    mkdir -p $SYSROOT/include/bits
    cp -rf arch/generic/bits/. $SYSROOT/include/bits/
    cp -rf arch/riscv64/bits/. $SYSROOT/include/bits/
    cp -f lib/crt1.o lib/crti.o lib/crtn.o lib/libc.a $SYSROOT/lib/
fi

# ---- 2. 内核 UAPI 头 ----
if [ ! -d $SYSROOT/usr/include/linux ]; then
    cd $SRC/linux-6.12
    make ARCH=riscv headers_install INSTALL_HDR_PATH=$SYSROOT/usr > /dev/null
fi

# ---- 3. riscv64 libgcc 相关的启动文件（compiler-rt 缺失的部分）----
# 装了交叉包时上面已经用 --gcc-install-dir 指过去了，这里什么都不用做。
# 没装才退回"下载 .deb 解包再把 GCC 库目录搬进 sysroot"的老路子（需要网络）。
if [ -z "$GCC_INSTALL" ] && { [ ! -s $SYSROOT/lib/libgcc.a ] || [ $(stat -c%s $SYSROOT/lib/libgcc.a) -lt 10000 ]; }; then
    cd /tmp && rm -rf rtdeb && mkdir rtdeb && cd rtdeb
    apt-get download libgcc-12-dev-riscv64-cross > /dev/null 2>&1
    dpkg -x *.deb x
    cp -f x/usr/lib/gcc-cross/riscv64-linux-gnu/12/libgcc.a \
          x/usr/lib/gcc-cross/riscv64-linux-gnu/12/libgcc_eh.a \
          x/usr/lib/gcc-cross/riscv64-linux-gnu/12/crtbegin.o \
          x/usr/lib/gcc-cross/riscv64-linux-gnu/12/crtbeginT.o \
          x/usr/lib/gcc-cross/riscv64-linux-gnu/12/crtend.o $SYSROOT/lib/ 2>/dev/null || true
fi
# musl 把数学库和 resolver 都并进了 libc.a，但 busybox 仍然会 -lm -lresolv。
# 补两个空归档让链接通过，符号照样从 libc.a 解析
for stub in libm.a libresolv.a; do
    if [ ! -f $SYSROOT/lib/$stub ]; then
        $AR_BIN rcs $SYSROOT/lib/$stub
    fi
done

# ---- 4. 编 busybox ----
cd $BB
make defconfig > /dev/null
sed -i 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config   # 6.12 UAPI 已删除 CBQ 定义
# clang 把 BB_GLOBAL_CONST（const 指针）当成不可变对象，会把
# SET_PTR_TO_GLOBALS 之后的读写都基于旧指针做（旧值为空），awk 等 applet
# 一启动就写 0x4 崩掉。关掉这个优化，生成代码才是顺序正确的
sed -i 's/^CONFIG_EXTRA_CFLAGS=.*/CONFIG_EXTRA_CFLAGS="-DBB_GLOBAL_CONST="/' .config
# telnetd：宿主机通过 slirp 后端的主机端口转发连进来（元件侧的 backend_opts.hostfwd）。
# defconfig 默认不开，它依赖 pty，init 里要挂 devpts
sed -i 's/^# CONFIG_TELNETD is not set$/CONFIG_TELNETD=y/' .config
sed -i 's/^# CONFIG_FEATURE_TELNETD_STANDALONE is not set$/CONFIG_FEATURE_TELNETD_STANDALONE=y/' .config
# 编译日志留档：失败时只打最后几十行（busybox 的 Kbuild 会把错误埋在一堆 clang
# 不认 GCC 参数的 warning 后面，全量刷屏反而看不见）
if ! make -j6 CC="$CC" AR=$AR_BIN RANLIB=$RANLIB_BIN STRIP=$STRIP_BIN HOSTCC=$CC_BIN > /tmp/busybox-make.log 2>&1; then
    tail -40 /tmp/busybox-make.log
    echo "busybox 编译失败，完整日志见 /tmp/busybox-make.log" >&2
    exit 1
fi

# ---- 5. initramfs ----
ROOT=/tmp/rootfs-rv
rm -rf $ROOT && mkdir -p $ROOT/{bin,sbin,proc,sys,dev,etc,tmp}
cp busybox $ROOT/bin/busybox && chmod +x $ROOT/bin/busybox
# 建立控制终端的小程序：脚本里的重定向拿不到 ctty
$CC -static -O2 -o $ROOT/bin/shell-init "$SCRIPT_DIR/dcs-shell-init.c"
cat > $ROOT/init <<'INIT'
#!/bin/sh
# 用绝对路径调 busybox：initramfs 里没有 /usr/bin，PATH 也不保证包含 /bin
/bin/busybox mount -t proc none /proc
/bin/busybox mount -t sysfs none /sys
/bin/busybox mount -t devtmpfs none /dev 2>/dev/null
# busybox 编了 400 多个 applet，但镜像里不给它们建链接的话，用户只能看到少数几个。
# --install -s 一次性在 /bin 建好全部软链接（vi/top/grep/ip/wget/...）
/bin/busybox --install -s /bin
export PATH=/bin:/sbin
echo ""
echo "[dcs] busybox init, 输入命令开始玩"
# 网络：slirp 固定分配 10.0.2.15/24，静态配好即可，不需要 dhcp 客户端
/bin/busybox mkdir -p /dev/pts
/bin/busybox mount -t devpts none /dev/pts
if /bin/busybox ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up 2>/dev/null; then
    # telnet 服务：宿主机经 slirp 的主机端口转发（backend_opts.hostfwd）连进来。
    # telnetd 每次连接 fork 一个 sh，要 pty，所以上面必须挂 devpts
    /bin/busybox telnetd -l /bin/sh -p 23
    echo "[dcs] telnetd 已启动（eth0 10.0.2.15:23）"
fi
# 内核交给 PID 1 的 fd 不绑定控制终端（/proc/1/stat 的 tty_nr 是 -1），ash 只在
# 进程有控制终端时才启用行编辑。脚本里的重定向拿不到控制终端，由这个小程序
# 自己 setsid + TIOCSCTTY 后再 exec sh
exec /bin/shell-init
INIT
chmod +x $ROOT/init
# mount 必须在符号链接列表里：漏掉它 devtmpfs 挂不上，sh 就拿不到 tty
for a in sh ls cat echo mount ps dmesg mkdir ln; do ln -sf busybox $ROOT/bin/$a; done
cd $ROOT && find . | cpio -o -H newc 2>/dev/null | gzip -9 > /tmp/initrd-busybox.cpio.gz
ls -la /tmp/initrd-busybox.cpio.gz
