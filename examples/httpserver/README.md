# ASP HTTP Server Examples

[中文](README.zh-CN.md)

These examples do not depend on `openasp-fpm`, Nginx, or external scripts.
`server.asp` implements the HTTP listener, request-header parsing, and response
framing.

## Start the Server

Run this command from the project root:

```sh
build/openasp examples/httpserver/server.asp \
    --root examples/httpserver \
    --query "host=127.0.0.1&port=18080"
```

Send requests:

```sh
curl -i http://127.0.0.1:18080/
curl -i http://127.0.0.1:18080/health
curl -I http://127.0.0.1:18080/
```

Press `Ctrl-C` to stop the server.

## Multiprocess Version

`multiprocess-server.asp` creates the listening socket before calling `Fork()`
to start multiple workers. The parent does not handle connections; it closes
its copy of the listener and reaps child processes.

```sh
build/openasp examples/httpserver/multiprocess-server.asp \
    --root examples/httpserver \
    --query "host=127.0.0.1&port=18080&workers=4" \
    --process-enable=on
```

`OpenASP.Process` is disabled by default, so this example requires
`--process-enable=on`. Each response includes an `X-OpenASP-Worker` header that
identifies the worker slot that handled the request.

## Parameters

Pass parameters through `--query`. Both examples support:

| Parameter | Default | Description |
| --- | --- | --- |
| `host` | `127.0.0.1` | Listen address; explicitly use `0.0.0.0` to accept external connections |
| `port` | `18080` | TCP port |

The examples also support these implementation-specific parameters:

| Parameter | Default | Description |
| --- | --- | --- |
| `maxRequests` | `0` | Exit `server.asp` after handling this many requests |
| `workers` | `4` | Number of forked worker processes, from 1 to 64 |
| `maxRequestsPerWorker` | `0` | Exit each worker after this many requests; `0` runs indefinitely |

Single-request verification example:

```sh
build/openasp examples/httpserver/server.asp \
    --root examples/httpserver \
    --query "host=127.0.0.1&port=18080&maxRequests=1"
```

## Scope

- HTTP/1.1 GET and HEAD.
- `/`, `/health`, and 404 responses.
- A 16 KiB request-header limit.
- Explicit `Content-Length`.
- Repeated sends to handle short socket writes.
- `Connection: close` on every response.

`server.asp` is a serial, single-process implementation.
`multiprocess-server.asp` accepts connections concurrently in multiple
processes. Both are instructional implementations and do not handle TLS,
chunked request bodies, keep-alive, or production-grade timeout management.
Continue to use `openasp-fpm` for production web services.
