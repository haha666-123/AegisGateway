<div align="center">

# Aegis · 高性能 LLM 网关

**C++20 · Linux epoll · 多线程 Reactor · OpenAI 兼容接口**

[![CI](https://github.com/haha666-123/AegisGateway/actions/workflows/ci.yml/badge.svg)](https://github.com/haha666-123/AegisGateway/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-lightgrey)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)
![License](https://img.shields.io/badge/license-MIT-green)

</div>

Aegis 是一个用 C++20 从零实现的 OpenAI 兼容网关：单进程、多线程 Reactor、零第三方依赖。
它把「客户端 → 网关 → 上游大模型」之间的协议细节、并发、流量治理和可观测性收拢到一层，
对上提供标准的 `/v1/chat/completions`（含流式 SSE），对下支持多上游路由、故障转移、
令牌桶限流、响应缓存和 Prometheus 指标。

> 本项目为个人项目，用于系统性地练习 Linux 网络编程、多线程模型与现代 C++ 工程化。
> 所有代码、单元测试、CI 都在仓库里可复现；没有引用任何无法验证的数据。

---

## 目录

- [为什么写这个项目](#为什么写这个项目)
- [功能特性](#功能特性)
- [系统架构](#系统架构)
- [快速开始](#快速开始)
- [配置](#配置)
- [HTTP 接口](#http-接口)
- [测试](#测试)
- [性能](#性能)
- [设计取舍与已知限制](#设计取舍与已知限制)
- [目录结构](#目录结构)
- [路线图](#路线图)
- [License](#license)

---

## 为什么写这个项目

工业上位机之外，我想证明自己能把「网络 + 并发 + 现代 C++ + AI 接入」这四件事放在一个
真正的服务里做完，而不是停留在示例代码：

| 能力 | 在本项目中的落点 |
| --- | --- |
| Linux 网络编程 | epoll 水平触发 Reactor；`accept` 独立线程；非阻塞 connect/read/write |
| 多线程 | 1 acceptor + N 个 I/O 线程（每线程一个 loop）+ 阻塞任务线程池 |
| 无锁并发 | 异步日志使用自研 MPSC 环形队列（序列号 + CAS，无锁、容量固定、满则计数丢弃） |
| 现代 C++ | C++20/23：`std::expected`、`std::format`、`std::jthread`、`std::span`、概念约束、`pmr` |
| 协议 | 自研增量式 HTTP/1.1 解析器（支持分片到达、chunked、pipeline、SSE 转发） |
| AI 接入 | OpenAI 兼容客户端、SSE 流式转发、多模型路由与故障转移、语义缓存 |
| 工程化 | CMake + Ninja + Presets、GitHub Actions（Linux GCC / ASan+UBSan / Windows MSVC）、单元测试、基准测试 |

---

## 功能特性

- **OpenAI 兼容**：`POST /v1/chat/completions`，支持 `stream: true`（SSE，逐 token 转发）。
- **多上游路由**：按模型名分组，加权轮询选择上游；失败自动切换下一个上游（故障转移），
  连续失败的上游进入冷却期（熔断）。
- **流量治理**：按 API Key 的令牌桶限流（令牌数 + 突发容量），超限返回 `429` 并带 `Retry-After`。
- **响应缓存**：精确匹配（模型 + 归一化消息）的 LRU + TTL 缓存；可选的语义缓存
  （向量余弦相似度，阈值可配），流式请求不缓存。
- **背压**：客户端写不动时，网关暂停上游读取（`pause_reading`），避免无界缓冲。
- **可观测**：`GET /metrics` 输出 Prometheus 文本格式（计数器 / 仪表 / 直方图，含 P50/P90/P99 桶）。
- **健壮性**：请求头/体大小限制、空闲连接超时、上游超时、优雅关闭（`SIGTERM`/`Ctrl-C`）。
- **零依赖**：JSON、HTTP、SSE、日志、指标、线程池全部手写，便于逐行讲清楚。

---

## 系统架构

```mermaid
flowchart LR
    Client[Client / SDK] -->|HTTP/1.1| Acceptor

    subgraph Aegis
        Acceptor[Acceptor thread<br/>accept + round robin]
        subgraph IO[I/O threads x N]
            L1[EventLoop #1<br/>epoll / WSAPoll]
            L2[EventLoop #2]
            L3[EventLoop #N]
        end
        Pool[ThreadPool<br/>DNS / 向量计算]
        Cache[(LRU + TTL<br/>语义缓存)]
        Metrics[/Prometheus metrics/]
        Acceptor --> L1
        Acceptor --> L2
        Acceptor --> L3
        L1 --> Cache
        L2 --> Cache
        L1 --> Pool
        L1 --> Metrics
    end

    L2 -->|HTTP/1.1 或 SSE| Up1[(上游 A<br/>api.openai.com)]
    L1 -->|故障转移| Up2[(上游 B<br/>自建 vLLM / Ollama)]

    Metrics --> Prom[GET /metrics]
```

线程模型：

```
main ──┬─ acceptor thread：只做 accept + 轮询派发（不跑业务）
       ├─ I/O thread #0：epoll/WSAPoll 事件循环，负责本线程内所有连接的读写与解析
       ├─ I/O thread #1 ...
       ├─ worker pool   ：getaddrinfo、语义向量计算等阻塞任务，结果 post 回 loop
       └─ log thread    ：从 MPSC 环形队列批量取日志，写文件并做滚动切分
```

一条连接从 accept 到关闭只会落在一个 I/O 线程上，所以连接状态（HTTP 解析器、缓冲区、
定时器）不需要加锁；跨线程只通过 `EventLoop::post()`（互斥保护 + socketpair 唤醒）交接。

---

## 快速开始

### Linux（主平台）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 单元测试
ctest --test-dir build --output-on-failure

# 启动网关（示例配置里有两个本地上游占位）
./build/bin/aegis --config config/gateway.json
```

### Windows / macOS

Windows 下 MinGW 构建会把 GCC 运行时（`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`）
自动复制到 `build/bin`，MSVC 构建会把 VC 运行时一并复制，双击 `build\bin\aegis.exe` 即可运行；
也可以加 `-DAEGIS_STATIC_RUNTIME=ON` 走静态链接，彻底不依赖 DLL。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

MinGW / Ninja：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\run.bat                  # 起假上游 + 网关，一键演示
.\deploy.bat               # 打包成 dist\：可执行文件 + 运行时 DLL + 配置
```

macOS 使用 `cmake -S . -B build -G Ninja && cmake --build build` 即可（Poller 自动退化为 `poll(2)`）。

### 冒烟测试

```bash
curl -s http://127.0.0.1:8080/healthz
curl -s http://127.0.0.1:8080/metrics | head -n 20

curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer sk-local' \
  -d '{"model":"gpt-4o-mini","stream":true,
       "messages":[{"role":"user","content":"用一句话解释 epoll"}]}'
```

没有真实上游时，可以用仓库里的 `tools/mock_upstream` 起一个本地假上游，用于端到端测试：

```bash
./build/bin/mock_upstream --port 9001 --delay-ms 20 --tokens 20
```

---

## 运行效果

本地一次完整起停（Windows 下 backend 为 `wsapoll`，Linux 下同一份代码走 `epoll`）：

```text
$ ./build/bin/mock_upstream --port 9001 --tokens 4 --delay-ms 10
mock upstream on http://127.0.0.1:9001/v1 (model=mock-1, tokens=4, delay=10ms)

$ ./build/bin/aegis --config config/local.json
[INFO] aegis 1.0.0 starting
[INFO] config: listen=127.0.0.1:8080 io_threads=2 upstreams=1 rate_limit=on cache=on
[INFO] gateway ready: 1 upstream(s), 2 io thread(s), cache enabled
[INFO] tcp server listening on port 8080 with 2 io threads (wsapoll)
[INFO] listening on 127.0.0.1:8080
[INFO] try: curl -s http://127.0.0.1:8080/healthz
```

非流式请求（携带缓存头语义：第二次相同请求命中进程内缓存，不再访问上游）：

```text
$ curl -s http://127.0.0.1:8080/v1/chat/completions \
    -H 'Authorization: Bearer sk-local' -H 'Content-Type: application/json' \
    -d '{"model":"mock-1","messages":[{"role":"user","content":"hi"}]}'
{"id":"chatcmpl-mock","object":"chat.completion","model":"mock-1",
 "choices":[{"index":0,"message":{"role":"assistant","content":"Hello from the mock upstream."},
             "finish_reason":"stop"}],
 "usage":{"prompt_tokens":6,"completion_tokens":4,"total_tokens":10}}
```

流式请求（网关边收边转，客户端按 SSE 逐帧消费）：

```text
$ curl -sN http://127.0.0.1:8080/v1/chat/completions \
    -H 'Authorization: Bearer sk-local' -H 'Content-Type: application/json' \
    -d '{"model":"mock-1","stream":true,"messages":[{"role":"user","content":"hi"}]}'

data: {"id":"chatcmpl-mock","object":"chat.completion.chunk",...,"content":"tok4 "}
data: {"id":"chatcmpl-mock","object":"chat.completion.chunk",...,"content":"tok3 "}
data: {"id":"chatcmpl-mock","object":"chat.completion.chunk",...,"content":"tok2 "}
data: {"id":"chatcmpl-mock","object":"chat.completion.chunk",...,"content":"tok1 "}
data: {"id":"chatcmpl-mock","object":"chat.completion.chunk","choices":[],
       "usage":{"prompt_tokens":6,"completion_tokens":4,"total_tokens":10}}
data: [DONE]
```

指标端点（Prometheus 文本格式）：

```text
$ curl -s http://127.0.0.1:8080/metrics | head -n 6
# HELP aegis_http_requests_total HTTP requests accepted by the gateway
# TYPE aegis_http_requests_total counter
aegis_http_requests_total 4
# HELP aegis_http_responses_total HTTP responses written by the gateway
# TYPE aegis_http_responses_total counter
aegis_http_responses_total 3
```

---

## 配置

配置为 JSON 文件（`--config`），环境变量可覆盖密钥：

```json
{
  "server":  { "host": "0.0.0.0", "port": 8080, "io_threads": 4, "max_connections": 20000,
               "idle_timeout_ms": 65000 },
  "logging": { "level": "info", "console": true, "file": "logs/aegis.log",
               "max_file_bytes": 33554432, "max_files": 4 },
  "limits":  { "max_header_bytes": 16384, "max_body_bytes": 4194304 },
  "upstreams": [
    { "name": "primary", "base_url": "https://api.openai.com", "api_key_env": "OPENAI_API_KEY",
      "models": ["gpt-4o-mini"], "weight": 3 },
    { "name": "backup",  "base_url": "http://127.0.0.1:9001", "api_key_env": "BACKUP_API_KEY",
      "models": ["gpt-4o-mini"], "weight": 1 }
  ],
  "rate_limit": { "enabled": true, "requests_per_second": 20, "burst": 40 },
  "cache": { "enabled": true, "capacity": 4096, "ttl_seconds": 300,
             "semantic": { "enabled": false, "threshold": 0.92 } },
  "timeouts": { "connect_ms": 3000, "upstream_ms": 30000 }
}
```

---

## HTTP 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/v1/chat/completions` | OpenAI 兼容；`stream=true` 时以 SSE 逐块转发 |
| `GET` | `/v1/models` | 返回配置中可用的模型与上游 |
| `GET` | `/healthz` | 存活探针，返回 `{"status":"ok"}` |
| `GET` | `/metrics` | Prometheus 文本格式指标 |
| `GET` | `/admin/stats` | 运行状态（连接数、缓存命中率、上游健康度） |

错误语义：请求体超限 `413`、请求头超限 `431`、限流 `429`、上游失败 `502`、上游超时 `504`。

---

## 测试

```bash
ctest --test-dir build --output-on-failure     # 68 个用例
./build/bin/aegis_tests --list                 # 列出用例
./build/bin/aegis_tests --filter=json          # 按名称过滤
```

覆盖范围：

- JSON：转义、Unicode/代理对、数字精度（`int64` 不丢位）、错误偏移、深层嵌套拒绝；
- HTTP：请求/响应解析、**在每个字节边界切分输入**、chunked、pipeline、大小限制、拒绝畸形报文；
- 并发：MPSC 队列 4 生产者压力测试（2 万条不丢不重）、令牌桶并发、线程池；
- 网络：事件循环（post / 定时器 / 取消）、socketpair、TCP echo、keep-alive 往返、连接回收；
- AI 层：SSE 分帧、token 估算、路由故障转移与熔断、缓存命中/过期、OpenAI 客户端（真实 TCP 往返）；
- 端到端：网关 + 假上游，覆盖鉴权、限流 429、缓存命中、流式转发、故障转移、请求体超限 413。

CI 在 Ubuntu 上跑 GCC 13（Release）与 ASan+UBSan（Debug），在 Windows 上跑 MSVC。
本地实测：68/68 通过（Windows MinGW GCC 13.1 与 Ubuntu GCC 13 各跑一遍）。

---

## 性能

> 数据由 `bench/bench.cpp` 测得，环境：AMD Ryzen 5 6600H（6C/12T）、Windows 11、
> MinGW GCC 13.1、`-O3 -DNDEBUG`、单进程。数字只用于横向比较本项目的几条路径，
> 不是对真实模型时延的承诺（真实时延由上游决定）。

```bash
./build/bin/aegis_bench --all
```

| 基准 | 结果 | 说明 |
| --- | --- | --- |
| `json_parse` | 18.3 万次/秒，约 210 MB/s | 约 1.2 KB 的 chat 请求体，含 8 条消息 |
| `http_request_parse` | 208 万次/秒，约 2.7 GB/s | 增量解析器，含头部解析与 body 拷贝 |
| `sse_parse` | 173 万次/秒，约 2.2 GB/s | 17 个事件的流，逐字节边界安全 |
| `tokenizer` | 560 万次/秒 | 启发式 token 估算 |
| `mpsc_queue` | 1685 万次/秒 | 4 生产者 1 消费者，200 万条无锁入队/出队 |
| `rate_limiter` | 2125 万次/秒 | 单键令牌桶（含互斥） |
| `end_to_end` | 2385 请求/秒 | 4 客户端线程 → 网关 1 个 I/O 线程 → 假上游，**每请求新连接、关闭缓存** |

`end_to_end` 刻意关闭了缓存并且不复用连接，因此它是"最坏情况"的下限；
开启缓存后重复请求不再走上游，吞吐由网关自身的 HTTP 处理决定。


---

## 设计取舍与已知限制

诚实说明，避免面试时被追问翻车：

- **边缘触发 vs 水平触发**：选择水平触发（LT）。所有连接都读到 `EAGAIN`，LT 更简单且不会
  漏事件；在本项目规模下 ET 带来的收益不足以抵消维护成本。
- **HTTP/1.1 only**：不支持 HTTP/2、WebSocket、TLS 卸载（生产上应由 Nginx/Envoy 终止 TLS）。
- **DNS 是异步的，但每次解析没有连接池复用**：上游连接按请求创建，尚未实现 keep-alive 连接池
  （已列入路线图）。这会带来额外的握手开销，但不影响功能正确性。
- **缓存是进程内的**：多实例部署时各自维护，命中率随实例数下降；跨实例共享需要 Redis 之类的外部存储。
- **令牌桶按 API Key 单机限流**：不是全局限流；多实例需要集中式配额。
- **token 数是估算值**：没有内置 BPE 词表，按「ASCII 4 字符 ≈ 1 token、CJK 1 字 ≈ 1 token」估算，
  仅用于限流与统计，不参与计费。

---

## 目录结构

```
src/
  common/   Status/Result、JSON、异步日志（MPSC）、指标、线程池、工具
  net/      Buffer、Socket、Poller(epoll/WSAPoll/poll)、EventLoop、Resolver、Http、TcpServer
  ai/       SSE 解析、OpenAI 客户端、加权路由+熔断、语义缓存、prompt 模板
  app/      Config、Gateway 路由、main
tests/      自研轻量测试框架 + 6 个测试翻译单元
bench/      解析/队列/端到端基准
config/     示例配置
scripts/    构建与 CI 辅助脚本
```

---

## 路线图

- [ ] 上游连接池（keep-alive 复用，降低握手开销）
- [ ] 流式响应的用量统计（usage 解析）
- [ ] 语义缓存接入真实 embedding 接口 + 向量索引（HNSW-lite）
- [ ] OpenTelemetry trace 导出
- [ ] 压测脚本（wrk/locust）与容量基线

---

## License

[MIT](LICENSE) © 2026 宋国辉
