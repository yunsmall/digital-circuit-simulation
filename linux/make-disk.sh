#!/bin/bash
# 生成 virtio-blk 用的空磁盘镜像（raw，未格式化）。
# guest 里可以用 mkfs.ext4/mkfs.vfat 自行格式化，或者在宿主上：
#   mke2fs -t ext4 -F images/disk.img
#
# 用法: make-disk.sh [输出路径] [大小]
set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$SCRIPT_DIR/../images/disk.img}
SIZE=${2:-16M}
mkdir -p "$(dirname "$OUT")"
truncate -s "$SIZE" "$OUT"
ls -la "$OUT"
