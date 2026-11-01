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
CC="$CC_BIN --target=$TARGET -march=rv64gc -mabi=lp64d --sysroot=$SYSROOT -fuse-ld=lld"

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

# ---- 3. riscv64 libgcc.a（compiler-rt 缺失的软浮点符号）----
if [ ! -s $SYSROOT/lib/libgcc.a ] || [ $(stat -c%s $SYSROOT/lib/libgcc.a) -lt 10000 ]; then
    cd /tmp && rm -rf rtdeb && mkdir rtdeb && cd rtdeb
    apt-get download libgcc-12-dev-riscv64-cross > /dev/null 2>&1
    dpkg -x *.deb x
    cp -f x/usr/lib/gcc-cross/riscv64-linux-gnu/12/libgcc.a $SYSROOT/lib/libgcc.a
fi

# ---- 4. 编 busybox ----
cd $BB
make defconfig > /dev/null
sed -i 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config   # 6.12 UAPI 已删除 CBQ 定义
make -j6 CC="$CC" AR=$AR_BIN RANLIB=$RANLIB_BIN STRIP=$STRIP_BIN HOSTCC=$CC_BIN > /dev/null

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
echo ""
echo "[dcs] busybox init, 输入命令开始玩"
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
