# ASP Server Module

[中文](README.zh-CN.md)

`asp.server` owns only server processes, FastCGI protocol adaptation, and
request dispatch. The ASP language runtime, compiler, VM, and client-side
network components remain the responsibility of their respective modules.

## File Responsibilities

- `fpm_command.eg`: `openasp-fpm` argument parsing, log initialization, and startup entry point.
- `server_config.eg`: Server runtime state shared by workers.
- `fastcgi_types.eg`: FastCGI request, response, and task DTOs.
- `fastcgi_codec.eg`: FastCGI record I/O, parameter decoding, and terminal-record encoding.
- `request_mapper.eg`: Maps FastCGI parameters to an ASP `RequestContext`.
- `response_encoder.eg`: Handles the HTML prefix, meta charset, response headers, and body encoding.
- `response_cache.eg`: Optional short-lived HTML response cache.
- `request_lifecycle.eg`: Creates, renders, commits, and transfers response ownership for one request.
- `fastcgi_connection.eg`: Connection reads, memory quotas, GC quiescent regions, and response sends.
- `worker.eg`: Concurrent connection tasks, worker GC configuration, and recycling.
- `master.eg`: Listening, AOT preparation, worker forks, monitoring, and restarts.
- `fastcgi_server.eg`: Compatibility facade for TCP and Unix socket startup APIs.

## Dependency Direction

```text
cmd/openasp_fpm
    -> asp.server
        -> asp
        -> asp.runtime
        -> protocol.fastcgi
        -> aio.tcp
        -> aio.unix.socket
```

The root `asp` module must not import `asp.server`. Neither `openasp` nor
`openasp-cli` should link FastCGI server symbols.

## Lifecycle Constraints

Connection handling must preserve this order:

1. Acquire the request memory quota and save the GC threshold.
2. Raise the threshold and enter the GC quiescent region.
3. Render the response and synchronously send the FastCGI header record.
4. Leave the request string arena and quiescent region, then restore the threshold.
5. Send the body asynchronously, release the response owner, and return the memory quota.
6. Send the terminal STDOUT record and `END_REQUEST`.

Cache writes must copy and promote strings within the response owner's request
string arena. Never cache a `StringView` or a temporary arena string.
