# P2P NAT 穿透 VPN 系统

一个基于 UDP Hole Punching 的 P2P 穿透 NAT 虚拟网络系统，采用 Client-Server 架构，支持 Linux 和 Windows 跨平台运行。

## 功能特性

- **UDP Hole Punching**：自动穿透 NAT，建立 P2P 直连
- **虚拟网卡**：创建 TUN 设备，分配虚拟 IP 地址
- **信令服务器**：协调客户端连接，维护在线状态
- **自动打洞**：客户端自动发现并连接所有已知 peer
- **心跳保活**：10 秒间隔心跳，30 秒超时检测
- **自动重连**：支持连接重置和重试机制
- **ARP 代理**：虚拟网络中的 MAC 地址解析
- **跨平台**：支持 Linux（/dev/net/tun）和 Windows（Wintun DLL）

## 架构说明

```
┌─────────────┐      ┌─────────────┐      ┌─────────────┐
│  Client A   │◄────►│   Server    │◄────►│  Client B   │
│  10.0.0.1   │      │  公网 IP    │      │  10.0.0.2   │
└─────────────┘      └─────────────┘      └─────────────┘
       │                    │                    │
       └────────────────────┴────────────────────┘
                    P2P 直连（UDP）
```

**工作流程：**
1. 客户端启动，创建 TUN 设备，分配虚拟 IP
2. 客户端向服务器发送 Login 消息，携带客户端 ID、虚拟 IP、MAC 地址
3. 服务器维护在线客户端列表，返回已知 peer 信息
4. 客户端请求服务器发起 Punch（打洞）请求
5. 服务器将双方公网地址互相通知（Punch Notify）
6. 双方互相发送 Punch Echo，完成 UDP 打洞
7. 打洞成功后，通过 TUN 设备直接传输 IP 数据包

## 构建和运行

### 构建

```bash
# 创建构建目录
mkdir -p build && cd build

# 生成 Makefile
cmake ..

# 编译
make
```

构建产物：
- `build/p2p_server` — 信令服务器
- `build/p2p_client` — P2P 客户端

### 运行服务器

```bash
# 默认监听端口 8800
./build/p2p_server

# 指定端口
./build/p2p_server 9000
```

### 运行客户端

```bash
# 格式: p2p_client <client_id> <virtual_ip> [server_ip] [server_port]
# 需要 root 权限（创建 TUN 设备）

# 客户端 A（在机器 1 上运行）：
sudo ./build/p2p_client c1 10.0.0.1 175.178.148.213 8800

# 客户端 B（在机器 2 上运行）：
sudo ./build/p2p_client c2 10.0.0.2 175.178.148.213 8800
```

### 运行测试

```bash
# 在服务器上先启动 p2p_server，然后：
python3 test_punch.py
```

## 使用示例

### 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `client_id` | 客户端唯一标识符（如 c1、c2） | 必填 |
| `virtual_ip` | 虚拟 IP 地址（类似 VPN 地址） | 必填 |
| `server_ip` | 信令服务器公网 IP | `127.0.0.1` |
| `server_port` | 信令服务器端口 | `8800` |

### 客户端行为

客户端启动后会自动：
1. 创建 TUN 设备（如 `tun_c1`）
2. 设置虚拟 IP 和 MTU（1400）
3. 登录服务器
4. 发现并打洞连接所有已知 peer
5. 发送心跳保活

### 验证连接

```bash
# 在客户端 A 上 ping 客户端 B 的虚拟 IP
ping 10.0.0.2

# 在客户端 B 上 ping 客户端 A 的虚拟 IP
ping 10.0.0.1
```

## 协议说明

项目使用自定义 UDP 协议，消息格式：

```
+--------+--------+--------+----------+--------+
| magic  |  type  |  seq   | body_len |  body  |
| 2byte  | 2byte  | 4byte  |  4byte   | N byte |
+--------+--------+--------+----------+--------+
```

**魔数：** `0xCAFE`

**消息类型：**

| 类型 | 代码 | 方向 | 用途 |
|------|------|------|------|
| MSG_LOGIN | 0x01 | C->S | 客户端登录 |
| MSG_LOGIN_RESP | 0x02 | S->C | 登录响应（含 peer 列表） |
| MSG_PUNCH_REQ | 0x03 | C->S | 请求打洞 |
| MSG_PUNCH_NOTIFY | 0x04 | S->C | 通知 peer 公网地址 |
| MSG_RESET_PUNCH | 0x05 | C->S | 请求重置连接 |
| MSG_RESET_NOTIFY | 0x06 | S->C | 重置通知（含新 session_id） |
| MSG_P2P_DATA | 0x07 | C->C | P2P 数据传输 |
| MSG_HEARTBEAT | 0x08 | C->S | 心跳 |
| MSG_HEARTBEAT_RESP | 0x09 | S->C | 心跳响应（含公网地址） |
| MSG_RESET_ACK | 0x0A | C->S | 重置确认 |
| MSG_PUNCH_ECHO | 0x0B | C->C | 打洞探测包 |
| MSG_PUNCH_ACK | 0x0C | C->C | 打洞确认 |

## 项目结构

```
nattc/
├── CMakeLists.txt           # CMake 构建配置
├── common.h                 # 协议定义、数据结构
├── utils.h                  # 工具函数声明
├── utils.c                  # 工具函数实现
├── server.h                 # 服务端接口声明
├── server.c                 # 服务端核心逻辑
├── main_server.c            # 服务端入口
├── client.h                 # 客户端接口声明
├── client.c                 # 客户端核心逻辑
├── main_client.c            # 客户端入口
├── test_punch.py            # Python 测试脚本
└── build/                   # 构建输出目录
```

## 技术特点

- **纯 C 实现**：无第三方依赖，仅使用 POSIX socket 和系统调用
- **非阻塞 I/O**：使用 `select()` 进行事件驱动
- **跨平台**：通过条件编译支持 Linux 和 Windows
- **完善的连接管理**：心跳保活、超时检测、自动重连

## 依赖

- **Linux**：POSIX socket、TUN 设备支持
- **Windows**：Winsock2、Wintun DLL（运行时加载）
- **测试脚本**：Python 3（仅使用标准库）

## 贡献

欢迎提交 Issue 和 Pull Request。

## 许可证

本项目未指定许可证，请联系作者获取许可信息。