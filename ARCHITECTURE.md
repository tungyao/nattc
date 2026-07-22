# NATTC 项目架构总览

## 项目概述
纯 C 实现的 P2P NAT 穿透 VPN 系统（本科毕业论文项目）。
- 语言：C99，CMake 构建
- 依赖：零外部依赖，仅使用 POSIX/Windows 系统 API
- 代码量：约 2461 行 C（7 个源文件 + 6 个头文件）+ 53 行 Python 测试

---

## 1. 目录结构

```
/root/server/nattc/
├── common.h              # 协议定义、数据结构、跨平台宏 (233行)
├── utils.h / utils.c     # 工具函数：UDP socket、序列化、IP转换 (33+202行)
├── server.h / server.c   # 信令服务器 (33+435行)
├── main_server.c         # 服务器入口 (41行)
├── client.h / client.c   # P2P 客户端 (121+1308行)
├── main_client.c         # 客户端入口 (55行)
├── CMakeLists.txt        # CMake 构建配置
├── test_punch.py         # 集成测试脚本
├── docs/
│   ├── event-loop-refactor.md     # select -> epoll 重构方案
│   └── lan-hole-punching.md       # 局域网打洞设计方案
├── build/                # 编译产物
└── README.md
```

---

## 2. 系统架构

```
┌─────────────┐          ┌─────────────┐
│  Client A   │◄─UDP hole─►│  Client B   │
│ (TUN设备)   │   punching  │ (TUN设备)   │
└──────┬──────┘          └──────┬──────┘
       │ UDP (信令)           │ UDP (信令)
       ▼                       ▼
┌─────────────────────────────────────┐
│          Signaling Server           │
│  (客户端管理/心跳保活/打洞协调)      │
└─────────────────────────────────────┘
```

**角色职责：**
- **Server**：信令服务器，维护在线客户端列表，协调打洞流程，不参与数据转发
- **Client**：P2P 客户端，创建 TUN 虚拟网卡，ARP 代理，UDP 打洞，P2P 数据转发

---

## 3. 通信协议设计 (common.h)

### 3.1 二进制包头 (12字节)
```
| magic(2B) | type(2B) | seq(4B) | body_len(4B) |
  0xCAFE (PROTO_MAGIC)
```
网络字节序（大端）编码，魔数 0xCAFE 用于协议识别与防误解析。

### 3.2 消息类型（16种）

| 代码 | 名称 | 方向 | 用途 |
|------|------|------|------|
| 0x01 | MSG_LOGIN | C→S | 客户端登录（V1） |
| 0x02 | MSG_LOGIN_RESP | S→C | 登录响应 + 在线对端列表 |
| 0x03 | MSG_PUNCH_REQ | C→S | 请求服务器协调打洞 |
| 0x04 | MSG_PUNCH_NOTIFY | S→C | 打洞通知（V1，仅公网地址） |
| 0x05 | MSG_RESET_PUNCH | C→S | 请求重置连接 |
| 0x06 | MSG_RESET_NOTIFY | S→C | 重置通知 + 新 session_id |
| 0x07 | MSG_P2P_DATA | C→C | P2P IP 数据包（含 session_id + seq） |
| 0x08 | MSG_HEARTBEAT | C→S | 心跳（10s间隔） |
| 0x09 | MSG_HEARTBEAT_RESP | S→C | 心跳响应（含服务器观察到的公网地址） |
| 0x0A | MSG_RESET_ACK | C→S | 重置确认（携带 notify_seq） |
| 0x0B | MSG_PUNCH_ECHO | C→C | 打洞探测包（含 peer_id + session_id） |
| 0x0C | MSG_PUNCH_ACK | C→C | 打洞应答（含 session_id） |
| 0x0D | MSG_LOGIN_V2 | C→S | 登录 V2（含局域网地址 local_addr） |
| 0x0E | MSG_LOGIN_RESP_V2 | S→C | 登录响应 V2（含对端局域网地址） |
| 0x0F | MSG_PUNCH_NOTIFY_V2 | S→C | 打洞通知 V2（含对端局域网地址） |
| 0x10 | MSG_RESET_NOTIFY_V2 | S→C | 重置通知 V2（含对端局域网地址） |

### 3.3 协议安全机制
- **魔数验证**：接收端强制检查 `magic != PROTO_MAGIC`，丢弃非法报文
- **序列号防重放**：P2P 数据包携带 `seq`，接收端维护 `rx_seq`，过滤重放包
- **局域网地址验证**：`is_valid_local_addr()` 检查 RFC1918 私有地址段，拒绝非法 local_addr
- **reset_notify 去重**：通过 `notify_seq` + `reset_ack_received` 防止重复处理同一重置通知

### 3.4 V1/V2 协议共存机制
- 客户端根据自身是否有有效的局域网地址，决定发送 `MSG_LOGIN`（V1）还是 `MSG_LOGIN_V2`（V2）
- 服务器检查除登录客户端外，是否存在任一在线客户端有非零 `local_addr`：
  - 若存在 → 使用 V2 消息（含局域网地址），回复 `MSG_LOGIN_RESP_V2`
  - 否则 → 使用 V1 消息，回复 `MSG_LOGIN_RESP`
- 打洞通知同理：`server_handle_punch_req()` 检查双方 `local_addr` 决定使用 V1 或 V2 notify
- V2 新增字段通过扩充结构体实现（如 `client_info_v2` 在末尾追加 `local_addr`），保持向前兼容

---

## 4. 服务器设计 (server.c)

### 4.1 核心数据结构
```c
server_context {
    int udp_fd;                    // UDP 监听 socket
    server_client *clients;        // 客户端链表
    uint32_t next_notify_seq;      // 通知序列号（递增分配）
}

server_client {
    char id[32];                   // 客户端 ID（字符串）
    uint32_t vip;                  // 虚拟 IP
    uint8_t mac[6];                // MAC 地址
    struct sockaddr_in public_addr;  // 公网地址（ip:port）
    struct sockaddr_in local_addr;   // 局域网地址（V2，0 表示无效）
    time_t last_heartbeat;         // 上次心跳时间
    server_client *next;
}
```

### 4.2 事件循环
`select()` 监听 UDP socket，1 秒超时：
1. 收到 UDP 数据包 → `server_handle_message()` 按消息类型分发
2. 超时 → `server_check_timeouts()` 清理 30s 未心跳的客户端

### 4.3 消息处理函数
| 函数 | 触发消息 | 处理逻辑 |
|------|---------|---------|
| `server_handle_login()` | MSG_LOGIN / MSG_LOGIN_V2 | 首次登录时 malloc 创建条目，重复登录时更新地址/心跳；检测 use_v2 标志后返回在线对端列表 |
| `server_handle_heartbeat()` | MSG_HEARTBEAT | 更新 `last_heartbeat`，返回 `MSG_HEARTBEAT_RESP`（含服务器观察到的公网地址，用于 NAT 类型感知） |
| `server_handle_punch_req()` | MSG_PUNCH_REQ | 查找请求方和目标方，根据双方 local_addr 决定 V1/V2，分别向双方发送对方地址的 notify |
| `server_handle_reset_punch()` | MSG_RESET_PUNCH | 为目标方分配新 `session_id` 和 `notify_seq`，发送 reset_notify；目标离线则发送零地址通知 |
| `server_handle_reset_ack()` | MSG_RESET_ACK | 当前版本接收但不处理（预留扩展） |

---

## 5. 客户端设计 (client.c)

### 5.1 核心数据结构
```c
client_context {
    int tun_fd;                    // TUN 设备文件描述符（Linux）
    int udp_fd;                    // UDP socket
    char tun_name[16];             // TUN 设备名
    char id[32];                   // 客户端 ID
    uint32_t vip;                  // 虚拟 IP
    uint8_t mac[6];                // MAC 地址（随机生成，固定 0x02 开头）
    struct sockaddr_in server_addr;  // 信令服务器地址
    struct sockaddr_in local_addr;   // 本机局域网地址（V2 自动发现）
    arp_entry *arp_table;          // ARP 缓存链
    peer_session *peers;           // 对等会话链表
    time_t last_heartbeat;         // 上次心跳发送时间
    time_t login_sent_time;        // 登录发送时间（超时 5s 则退出）
    int login_received;            // 是否已收到登录响应
    // Windows 扩展
    wintun_ctx wintun;             // Wintun 适配器句柄
}

peer_session {
    char id[32];                   // 对端 ID
    uint32_t vip;                  // 对端虚拟 IP
    uint8_t mac[6];                // 对端 MAC
    struct sockaddr_in public_addr;  // 对端公网地址
    struct sockaddr_in local_addr;   // 对端局域网地址
    enum peer_state state;         // 会话状态
    uint32_t session_id;           // 当前会话 ID（随机生成）
    uint32_t tx_seq;               // 发送序号
    uint32_t rx_seq;               // 接收序号（防重放）
    time_t last_rx_time;           // 最后收包时间
    time_t last_tx_time;           // 最后发包时间
    int punch_attempts;            // 当前阶段打洞尝试次数
    time_t last_punch_time;        // 上次打洞探测时间
    int lan_phase;                 // 打洞阶段（LAN_PHASE_NONE/LAN/LAN）
    uint32_t reset_notify_seq;     // 已处理的 reset 通知序号（去重）
    int reset_ack_received;        // 是否已收到 reset ack
    int reset_retries;             // reset 重试次数
    time_t last_reset_time;        // 上次 reset 时间
    peer_session *next;
}

arp_entry {
    uint32_t vip;                  // 虚拟 IP
    uint8_t mac[6];                // 对应 MAC
    char peer_id[32];              // 对端 ID
    time_t last_seen;              // 最近活跃时间
    arp_entry *next;
}
```

### 5.2 初始化流程
1. 生成随机 MAC（首字节 0x02 表示本地管理地址）
2. 创建 TUN 虚拟网卡（`tun_create`）
3. 设置 TUN IP/子网掩码/MTU
4. 创建 UDP socket
5. 调用 `client_discover_local_addr()` 自动发现本机局域网地址
6. 发送登录请求（根据 local_addr 有效性选择 V1/V2）

### 5.3 事件循环
`select()` 监听 UDP socket + TUN 文件描述符，100ms 超时（Linux）/ `WaitForSingleObject` + 50ms 超时（Windows）：

```
while (running):
    1. 读 UDP socket → client_handle_message() 按消息类型分发
    2. 读 TUN 设备   → client_process_tun_packet() → ARP 查询 → P2P 转发
    3. 定时任务（每秒）：
       - 心跳发送（10s 间隔）
       - 登录超时检查（5s 未响应则退出进程）
       - 打洞状态更新（client_update_punch_state）
       - 保活探测（5s 间隔）
       - 对端超时检测（15s 无数据则发起 reset）
       - Peer 状态摘要打印（每 5s）
```

### 5.4 消息处理函数
| 函数 | 触发消息 | 处理逻辑 |
|------|---------|---------|
| `client_handle_login_resp()` | MSG_LOGIN_RESP | 解析在线对端列表到 peer_session 链，逐一对每个对端发送 `MSG_PUNCH_REQ` |
| `client_handle_login_resp_v2()` | MSG_LOGIN_RESP_V2 | 同上，且解析对端的 local_addr 字段 |
| `client_handle_punch_notify()` | MSG_PUNCH_NOTIFY / V2 | 创建/更新 peer_session，填充地址，调用 `client_start_punching()` |
| `client_handle_reset_notify()` | MSG_RESET_NOTIFY / V2 | 去重检查（`reset_notify_seq`），清除旧映射，更新 session_id，重新打洞或置为 IDLE |
| `client_handle_punch_echo()` | MSG_PUNCH_ECHO | 若 punching 状态：回复 PUNCH_ACK，切换为 ESTABLISHED，发送反向确认 echo |
| `client_handle_punch_ack()` | MSG_PUNCH_ACK | 查找 punching 状态的 peer，切换为 ESTABLISHED，发送反向确认 echo |
| `client_handle_p2p_data()` | MSG_P2P_DATA | 按 session_id 匹配 peer，`rx_seq` 防重放，写入 TUN 设备 |
| `client_handle_heartbeat_resp()` | MSG_HEARTBEAT_RESP | 打印服务器观察到的公网地址 |

### 5.5 TUN 虚拟网卡管理
- **Linux**：打开 `/dev/net/tun`，`ioctl(TUNSETIFF)` 创建 `tun_<client_id>` 设备；`ioctl(SIOCSIFADDR/SIOCSIFNETMASK)` 设置 IP，`ioctl(SIOCSIFMTU)` 设置 MTU
- **Windows**：运行时 `LoadLibraryW("wintun.dll")` + `GetProcAddress` 加载 Wintun API，调用 `WintunCreateAdapter` / `WintunStartSession` 创建适配器，注册 `read_event` 用于非阻塞读取；通过 `netsh` 或 Wintun API 配置 IP
- **路由管理**：Linux 使用 `ioctl(SIOCADDRT/SIOCDELRT)` 系统调用维护内核路由表，Windows 使用 Wintun 内置路由机制
- **MTU**：统一设置为 1400 字节（TUN_MTU），为 UDP 封装预留空间

### 5.6 ARP 代理
- 内核发起 ARP 请求查询虚拟 IP 时，ARP 报文经 TUN 设备上送到用户态
- `client_process_tun_packet()` 检测到 ARP 包（EtherType 0x0806），调用 `client_send_arp_reply()`
- 查询 `arp_table`：若找到 VIP 对应的 MAC，构造 ARP Reply 写回 TUN（源 MAC 为对端 MAC，目标 MAC 为本机 MAC）
- ARP 表维护：登录响应和打洞通知时通过 `arp_add()` 自动填充，连接断开时通过 `arp_clear_peer()` 清理

### 5.7 打洞引擎 — 两阶段策略

**阶段一：LAN 优先探测**
- 条件：对端 `local_addr` 非零且不同于公网地址
- 参数：最多 `LAN_PUNCH_ATTEMPTS_MAX=5` 次，间隔 `LAN_PUNCH_INTERVAL_MS=200ms`
- 超时：`LAN_PUNCH_TIMEOUT_SEC=3s` 后切换至 WAN 阶段
- 目标地址：使用对端 `local_addr` 但端口取 `public_addr.sin_port`（防止本地端口不同于公网端口）

**阶段二：WAN 回退**
- 首轮爆发：`PUNCH_ATTEMPTS_FIRST=5` 次，间隔 `PUNCH_INTERVAL_FIRST_MS=200ms`
- 后续持续：间隔 `PUNCH_INTERVAL_MS=1000ms`
- 总超时：`PUNCH_TIMEOUT_SEC=30s` 后放弃，peer 恢复 IDLE
- 目标地址：对端 `public_addr`

**打洞握手**：
1. A 向 B 发送 `MSG_PUNCH_ECHO`（含 session_id）
2. B 收到后回复 `MSG_PUNCH_ACK`（含 session_id），切换 ESTABLISHED
3. B 再发送一次 `MSG_PUNCH_ECHO` 给 A（反向确认）
4. A 收到 B 的 ACK 后切换 ESTABLISHED，也发送一次 `MSG_PUNCH_ECHO`

**保活机制**：
- 已建立连接每 `PEER_KEEPALIVE_INTERVAL=5s` 发送 `MSG_PUNCH_ECHO`
- 支持双路径保活：同时向 public_addr 和 local_addr 发送 keepalive（若 LAN 地址存在且不同于公网地址）
- 超时检测：`PEER_TIMEOUT_SEC=15s` 未收包则触发 `client_initiate_reset()`

### 5.8 对等会话状态机
```
IDLE ──(收到打洞通知)──→ PUNCHING ──(收到 ACK)──→ ESTABLISHED
  ↑                        ↓                         │
  │                    (30s超时)                      │
  │                        ↓                         │
  └──────────────────── IDLE                         │
                                                     │
                                              (15s无数据/主动reset)
                                                     ↓
                      IDLE ←(reset完成)── RESETTING ←┘
```

### 5.9 Reset 协议
- **发起**：客户端检测到 peer 超时或主动断开，调用 `client_initiate_reset()`
  - 清除 ARP 映射和内核路由（`peer_clear_all_mappings`）
  - 发送 `MSG_RESET_PUNCH` 给服务器
- **服务器中转**：分配新 `notify_seq` 和新 `session_id`，发送 `MSG_RESET_NOTIFY` 给对端
  - 若对端已离线：返回零地址 notify，触发本地置 IDLE
  - 若对端在线：通知其更新地址信息并重新打洞
- **去重机制**：客户端维护 `reset_notify_seq`，重复收到相同序号直接回复 ACK 跳过处理
- **确认**：客户端收到 reset_notify 后发送 `MSG_RESET_ACK` 给服务器

### 5.10 局域网地址自动发现
`client_discover_local_addr()` 在客户端初始化时调用：
- Linux：使用 `getifaddrs()` 遍历网卡，取默认路由接口的第一个非 loopback 地址，验证其属于 RFC1918 私有地址段
- Windows：使用 `GetAdaptersAddresses()` 遍历适配器，跳过 loopback 和 TUN 接口
- 验证函数 `is_rfc1918()`：检查地址是否属于 10.x、172.16-31.x、192.168.x 段
- 若未发现有效的局域网地址，`local_addr` 保持全零，客户端使用 V1 协议

---

## 6. 跨平台设计

### 6.1 平台抽象
通过 `#ifdef _WIN32` 条件编译抽象以下差异：

| 特性 | Linux | Windows |
|------|-------|---------|
| Socket 操作 | `socket/close` | `WSASocket/closesocket`（宏 `sock_close`） |
| 非阻塞 | `fcntl(O_NONBLOCK)` | `ioctlsocket(FIONBIO)` |
| 错误码 | `errno` | `WSAGetLastError()`（宏 `sock_errno`） |
| TUN 设备 | `/dev/net/tun` + `ioctl` | Wintun DLL 运行时加载 |
| 网络接口枚举 | `getifaddrs()` | `GetAdaptersAddresses()` |
| 路由管理 | `ioctl(SIOCADDRT)` | Wintun 内置 |
| 进程退出 | `SIGINT/SIGTERM` 信号处理 | `SetConsoleCtrlHandler` |
| 随机数种子 | `getpid()` | `GetCurrentProcessId()` |

### 6.2 Windows 特殊处理
- **`SIO_UDP_CONNRESET` 禁用**（`utils.c`）：Windows 的 ICMP Port Unreachable 错误会导致 `recvfrom` 返回 `WSAECONNRESET`（10054），须在 UDP socket 上用 `WSAIoctl` 设置 `SIO_UDP_CONNRESET` 以禁用此行为，否则打洞期间对端 NAT 路由未建立时会被此错误中断
- **Wintun 运行时加载**：通过 `LoadLibraryW("wintun.dll")` + `GetProcAddress` 动态获取所有 Wintun 函数指针，避免编译期依赖
- **Wintun read_event 轮询**：事件循环中用 `WaitForSingleObject(read_event, 0)` 先检查数据是否就绪，然后再调用非阻塞读取，避免 Wintun 在无数据时阻塞

---

## 7. 构建与运行

```bash
mkdir build && cd build
cmake ..
make
# 产物：build/p2p_server、build/p2p_client

./p2p_server [port]                          # 启动服务器（默认 8800）
./p2p_client <id> <vip> [server] [port]       # 启动客户端
```

---

## 8. Git 提交历史

```
0de3a2f feat: 支持局域网内部打洞 (LAN hole punching)
1020af0 docs: 添加 README.md 项目文档
6bb4291 init
```

---

## 9. 论文写作要点

1. **NAT 穿透原理**：UDP 打洞的协议交互与状态机设计
2. **两阶段打洞策略**：LAN 优先 + WAN 回退，提升局域网场景连接速度
3. **虚拟网络层**：TUN 设备 + ARP 代理 + `ioctl(SIOCADDRT)` 路由管理
4. **自定义二进制协议**：12 字节统一包头设计、16 种消息类型定义、网络字节序列化
5. **安全机制**：魔数校验防误识别、seq 防重放、RFC1918 私网地址验证、notify_seq 去重
6. **V1/V2 协议演进**：局域网地址感知的消息版本协商，新旧客户端混合场景降级兼容
7. **跨平台可移植性**：条件编译抽象 Linux/Windows 差异，Wintun 运行时加载，`SIO_UDP_CONNRESET` 处理
8. **系统轻量化**：零外部依赖、纯 C 实现、2461 行代码、<5MB 运行时内存
