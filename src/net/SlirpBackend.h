// libslirp 用户态 NAT。实现成 VirtioNet 的后端表，backend 参数写 "slirp" 即可。
//
// 上下文由 create 申请、destroy 释放：
//   create 建 slirp 实例并起一个后台线程跑事件循环（socket 收发和定时器），
//   destroy 通知线程退出并回收。
// 数据面在宿主主线程里通过 recv_pkt/send_pkt 和两个队列交换帧，
// slirp 本身只在后台线程里访问。
#pragma once
#include "dcs/components/platform/VirtioNet.h"

namespace dsc::net {

VirtioNet::BackendOps slirpBackendOps();

} // namespace dsc::net
