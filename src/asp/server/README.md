# ASP Server 模块

`asp.server` 只负责服务端进程、FastCGI 协议适配和请求调度。ASP 语言运行时、编译器、VM 与客户端网络组件仍由各自模块负责。

## 文件职责

- `fpm_command.eg`：`openasp-fpm` 参数解析、日志初始化和启动入口。
- `server_config.eg`：worker 共享的服务端运行状态。
- `fastcgi_types.eg`：FastCGI 请求、响应和任务 DTO。
- `fastcgi_codec.eg`：FastCGI record 收发、参数解码和结束帧编码。
- `request_mapper.eg`：将 FastCGI 参数映射为 ASP `RequestContext`。
- `response_encoder.eg`：HTML 前缀、meta charset、响应头和正文编码处理。
- `response_cache.eg`：可选的短期 HTML 响应缓存。
- `request_lifecycle.eg`：单次请求的上下文创建、渲染、提交和响应所有权转移。
- `fastcgi_connection.eg`：连接读取、内存配额、GC quiescent 区间和响应发送。
- `worker.eg`：连接任务并发、worker GC 配置和回收。
- `master.eg`：监听、AOT 准备、worker fork、监控和重启。
- `fastcgi_server.eg`：提供 TCP 与 Unix socket 启动 API 的兼容 facade。

## 依赖方向

```text
cmd/openasp_fpm
    -> asp.server
        -> asp
        -> asp.runtime
        -> protocol.fastcgi
        -> aio.tcp
        -> aio.unix.socket
```

根模块 `asp` 不得反向 import `asp.server`。`openasp` 和 `openasp-cli` 不应链接 FastCGI server 符号。

## 生命周期约束

连接处理中的以下顺序不可调整：

1. 获取请求内存配额并保存 GC threshold。
2. 提高 threshold，进入 GC quiescent 区间。
3. 渲染响应并同步发送 FastCGI header record。
4. 离开 request string arena 和 quiescent 区间，恢复 threshold。
5. 异步发送 body，清理 response owner，释放内存配额。
6. 发送 STDOUT 结束帧和 END_REQUEST。

缓存写入必须在 response owner 的 request string arena 中复制并提升字符串，禁止缓存 `StringView` 或 arena 临时字符串。
