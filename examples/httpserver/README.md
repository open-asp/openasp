# ASP HTTP 服务器示例

该示例不依赖 `openasp-fpm`、Nginx 或外部脚本，HTTP 监听、请求头解析和响应封包全部由 `server.asp` 完成。

## 启动

在项目根目录执行：

```sh
build/openasp examples/httpserver/server.asp \
    --root examples/httpserver \
    --query "host=127.0.0.1&port=18080"
```

访问：

```sh
curl -i http://127.0.0.1:18080/
curl -i http://127.0.0.1:18080/health
curl -I http://127.0.0.1:18080/
```

停止服务器时按 `Ctrl-C`。

## 多进程版本

`multiprocess-server.asp` 会先创建监听 socket，再通过 `Fork()` 启动多个
worker。父进程不处理连接，只负责关闭自己的监听副本并回收子进程。

```sh
build/openasp examples/httpserver/multiprocess-server.asp \
    --root examples/httpserver \
    --query "host=127.0.0.1&port=18080&workers=4" \
    --process-enable=on
```

`OpenASP.Process` 默认禁用，因此该示例必须显式传入
`--process-enable=on`。每个响应的 `X-OpenASP-Worker` 头会标识处理请求的
worker 槽位。

## 参数

参数通过 `--query` 传入。两个示例都支持：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `host` | `127.0.0.1` | 监听地址；需要对外开放时显式设为 `0.0.0.0` |
| `port` | `18080` | TCP 端口 |

单进程版本另外支持 `maxRequests`；多进程版本使用下面两个参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `maxRequests` | `0` | `server.asp` 处理指定数量请求后退出 |
| `workers` | `4` | fork 的 worker 进程数，范围为 1 至 64 |
| `maxRequestsPerWorker` | `0` | 每个 worker 处理指定数量请求后退出；`0` 表示持续运行 |

单次验证示例：

```sh
build/openasp examples/httpserver/server.asp \
    --root examples/httpserver \
    --query "host=127.0.0.1&port=18080&maxRequests=1"
```

## 实现范围

- HTTP/1.1 GET 和 HEAD。
- `/`、`/health` 及 404 响应。
- 16 KiB 请求头上限。
- 显式 `Content-Length`。
- 循环发送以处理 socket 短写。
- 每个响应使用 `Connection: close`。

`server.asp` 是单进程串行版本；`multiprocess-server.asp` 使用多个进程并发
接收连接。两者都属于教学实现，不处理 TLS、chunked request body、keep-alive
或生产级超时治理。正式 Web 服务应继续使用 `openasp-fpm`。
