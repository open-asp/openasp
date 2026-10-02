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

## 参数

参数通过 `--query` 传入：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `host` | `127.0.0.1` | 监听地址；需要对外开放时显式设为 `0.0.0.0` |
| `port` | `18080` | TCP 端口 |
| `maxRequests` | `0` | 处理指定数量请求后退出；`0` 表示持续运行 |

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

该示例是单进程、单连接串行教学实现，不处理 TLS、chunked request body、keep-alive、并发连接或生产级超时治理。正式 Web 服务应继续使用 `openasp-fpm`。
