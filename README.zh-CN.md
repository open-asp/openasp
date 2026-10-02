# OpenASP：让 Classic ASP 运行在 Linux、Unix 和 MacOS

<p align="center" id="openasptop">
  <img src="assets/openasp-banner.png" alt="OpenASP" width="500">
</p>

<p align="center">
  <a href="#从源码构建"><img src="assets/badge-compiler.png" alt="编译器：Egret 0.1.6" height="20"></a>
  <a href="LICENSE"><img src="assets/badge-license.png" alt="许可证：MIT" height="20"></a>
  <a href="#告别-windows-与-iis-限制"><img src="assets/badge-platforms.png" alt="平台：MacOS 与 Linux" height="20"></a>
</p>

<p align="center">
  <a href="#通过二进制安装包安装">安装</a> |
  <a href="#从源码构建">构建</a> |
  <a href="#hello-world">Hello World</a> |
  <a href="#将-openasp-fpm-部署在-web-server-之后">部署</a> |
  <a href="#classic-asp-兼容能力">兼容能力</a> |
  <a href="#openasp-扩展组件">扩展组件</a> |
  <a href="#架构">架构</a> |
  <a href="docs/OpenASP命令行.md">命令行</a> |
  <a href="https://openasp.dev">官方网站</a> |
  <a href="README.md">English</a>
</p>

项目官方主页：**[https://openasp.dev](https://openasp.dev)**。如有任何问题、建议或其他反馈，请访问该网站。

传统 Classic ASP 应用长期依赖 **Windows Server 和 IIS** 才能运行。OpenASP 打破了这一平台限制，让现有 VBScript 与 ASP 应用无需 Windows 和 IIS，也能原生运行在 **Linux、Unix 和 MacOS** 上。

OpenASP 由 Egret 语言前端和原生 C 运行时组成，内置 FastCGI 服务、ADO 兼容数据访问、字节码执行与 AOT mmap 加载。兼容性、性能优化和系统适配全部在引擎层完成，不要求业务 ASP 编写平台专用的变通逻辑。

可以直接[安装二进制发行包](#通过二进制安装包安装)，也可以
[从源码构建](#从源码构建)。安装后从[第一个 ASP 页面](#hello-world)开始，
再将 [`openasp-fpm` 部署在 Web server 之后](#将-openasp-fpm-部署在-web-server-之后)。

## 告别 Windows 与 IIS 限制

| 传统 Classic ASP | OpenASP |
| --- | --- |
| 必须依赖 Windows Server | 可运行于 Linux、Unix 和 MacOS |
| 由 IIS 托管 | 可独立运行，也可部署于 Nginx/FastCGI 之后 |
| 依赖 Windows 平台的 COM 与 ADO | 使用原生 C 组件和进程内数据库驱动 |
| 跨平台迁移需要改写业务代码 | 兼容逻辑统一由运行时实现 |

## 通过二进制安装包安装

根据操作系统和 CPU 架构下载 OpenASP 0.1.1 安装包：

| 平台 | 二进制安装包 |
| --- | --- |
| Linux x86-64 | [`openasp-0.1.1-linux-x86-64.tar.gz`](https://github.com/open-asp/openasp/releases/download/pre-release-0.1.1/openasp-0.1.1-linux-x86-64.tar.gz) |
| Linux ARM64 | [`openasp-0.1.1-linux-arm64.tar.gz`](https://github.com/open-asp/openasp/releases/download/pre-release-0.1.1/openasp-0.1.1-linux-arm64.tar.gz) |
| MacOS Apple Silicon | [`openasp-0.1.1-macos-arm64.zip`](https://github.com/open-asp/openasp/releases/download/pre-release-0.1.1/openasp-0.1.1-macos-arm64.zip) |

每个安装包的 `bin/` 目录中包含三个程序：

- `openasp`：直接运行 ASP 页面。
- `openasp-cli`：启动交互式命令行环境。
- `openasp-fpm`：启动 FastCGI 服务。

下载后解压：

```sh
# Linux
tar -xzf openasp-0.1.1-linux-x86-64.tar.gz

# MacOS
unzip openasp-0.1.1-macos-arm64.zip
```

将解压目录中的 `bin/` 加入 `PATH`：

```sh
export PATH="/path/to/openasp-0.1.1-linux-x86-64/bin:$PATH"
openasp --help
```

Linux ARM64 或 MacOS 使用对应的解压目录名。需要永久生效时，将 `export`
命令加入 shell 配置文件。

## 从源码构建

OpenASP 当前从源码构建，并依赖 Egret 编译器源码树。构建过程会生成自举编译器、
打过补丁的运行时、原生 C 库以及三个 OpenASP 可执行程序。

### 安装 Egret 编译器

编译 OpenASP 或 `egret-web-server` 前先安装 Egret 0.1.6。各平台工具链可从
[egret-lang 发布目录](https://github.com/egret-lang/release/tree/main/egret-lang)
获取：

| 主机 | 软件包 |
| --- | --- |
| MacOS Apple Silicon | `egret-0.1.6-aarch64-apple-darwin.zip` |
| Linux x86_64 | `egret-0.1.6-x86_64-unknown-linux-gnu.tar.gz` |
| Linux ARM64 | `egret-0.1.6-aarch64-unknown-linux-gnu.tar.gz` |

下载并解压对应平台的软件包，将 `EGRET_HOME` 指向解压目录，并把
`$EGRET_HOME/bin` 加入 `PATH`。执行 `egret version` 可以确认安装是否成功。

二进制包提供编译器和标准文件。编译 OpenASP 还需要版本匹配的完整 Egret
源码树，执行 `make` 前必须将 `EGRET_ROOT` 指向该源码目录。

### Linux 依赖

Ubuntu 24.04：

```sh
sudo apt-get update
sudo apt-get install -y \
    bison build-essential ca-certificates clang curl file flex \
    libglib2.0-dev libpcre2-dev libssl-dev libsqlite3-dev \
    libxml2-dev libzstd-dev lld llvm-dev mdbtools mdbtools-dev \
    nasm patch pkg-config python3 zlib1g-dev
```

其他 Linux 和 Unix 发行版需要安装对应软件包。链接阶段必须能够找到静态版本的
`libmdb`、`libmdbsql`、SQLite、PCRE2、OpenSSL Crypto 和 zlib，同时需要
C11 编译器与 LLVM 工具链。

### MacOS 依赖

安装 Xcode 命令行工具和 Homebrew 软件包：

```sh
xcode-select --install
brew install \
    bison flex glib libiconv libxml2 llvm lld mdbtools nasm \
    openssl@3 pcre2 pkg-config python sqlite zstd

export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
```

MacOS 构建会静态链接 GNU `libiconv` 和 `libcharset`。如果 Homebrew 不在
标准路径，可向 `make` 传入
`GNU_ICONV_PREFIX="$(brew --prefix libiconv)"`。

### 构建

将 `EGRET_ROOT` 指向包含 `egret/`、`compiler/`、`runtime/` 和 `include/`
的 Egret 源码目录，然后执行：

```sh
export EGRET_ROOT=/path/to/egret
make clean
make EGRET_ROOT="$EGRET_ROOT" build
```

构建产物位于 `build/`：

| 程序 | 用途 |
| --- | --- |
| `openasp` | 直接执行或编译 ASP 页面 |
| `openasp-cli` | 保留 Session/Application 状态的交互式环境 |
| `openasp-fpm` | 面向 Nginx 等网关的 FastCGI 服务 |

如果库安装路径无法由 `pkg-config` 找到，可使用 Makefile 提供的
`MDBTOOLS_PREFIX`、`SQLITE_PREFIX`、`OPENSSL_PREFIX`、
`GNU_ICONV_PREFIX` 或 `PCRE2_LIBDIR` 覆盖路径。

## Hello World

创建 `/tmp/openasp-site/default.asp`：

```asp
<%@ Language=VBScript %>
<%
Response.ContentType = "text/html; charset=utf-8"
Dim name
name = Request.QueryString("name")
If name = "" Then name = "World"
%>
<!doctype html>
<html>
<head><meta charset="utf-8"><title>OpenASP</title></head>
<body><h1>Hello, <%= Server.HTMLEncode(name) %>!</h1></body>
</html>
```

不启动 Web 服务器，直接渲染该页面：

```sh
mkdir -p /tmp/openasp-site
build/openasp /tmp/openasp-site/default.asp \
    --root /tmp/openasp-site \
    --query "name=OpenASP"
```

`openasp` 支持以下请求和运行时参数：

| 参数 | 作用 |
| --- | --- |
| `<page.asp>` 或 `--file <page.asp>` | ASP 入口文件 |
| `--root <dir>` | 站点根目录；默认使用入口文件所在目录 |
| `--query <a=1>` | URL 编码的查询字符串 |
| `--body <x=1>` | 请求正文 |
| `--method <GET\|POST>` | HTTP 方法，默认为 `GET` |
| `--session <id>` | 多次本地调用使用的固定 Session ID |
| `--log-file <path\|off>` | 日志基础路径，或关闭文件日志 |
| `--log-level <level\|off>` | `info`、`notice`、`warn`、`error` 或 `off` |
| `--shell-enable=on` | 开启高权限 `Shell.Application`，默认关闭 |
| `--process-enable=on` | 开启高权限 `OpenASP.Process`，默认关闭 |

`openasp-cli --root /path/to/site` 会启动交互式 VBScript 环境，变量、
`Session` 和 `Application` 会在命令之间保留；脚本化输入可使用
`--no-prompt`。

## 将 openasp-fpm 部署在 Web server 之后

### 1. 启动 OpenASP FastCGI

```sh
build/openasp-fpm \
    --root /srv/www/example \
    --host 127.0.0.1 \
    --port 9000 \
    --workers 4 \
    --worker-connections 128 \
    --max-accepts 1000 \
    --log-file /var/log/openasp/fpm.log \
    --log-level info
```

| 参数 | 默认值 | 作用 |
| --- | --- | --- |
| `--root <dir>` | `.` | 站点物理根目录 |
| `--host <address>` | `127.0.0.1` | FastCGI 监听地址 |
| `--port <port>` | `9000` | FastCGI TCP 端口 |
| `--workers <n>` | `2` | worker 进程数 |
| `--worker-connections <n>` | `128` | 每个 worker 的 FastCGI 并发连接数 |
| `--max-accepts <n>` | `0` | 每接受 `n` 个连接后回收 worker；`0` 表示不按连接数回收 |
| `--aot=on\|off` | `on` | 启用 mmap AOT 页面镜像 |
| `--log-file <path\|off>` | `logs/openasp-fpm.log` | 按日日志基础路径 |
| `--log-level <level\|off>` | `info` | 日志级别 |
| `--shell-enable=on` | `off` | 开启 `Shell.Application` |
| `--process-enable=on` | `off` | 开启 `OpenASP.Process` |

日志文件名会自动追加 `-YYYYMMDD`，父目录不存在时会自动创建。请求日志不会记录
查询字符串、请求正文、Cookie、Session 值和 OpenAI API Key。

### 2A. 配置 Nginx

创建 `/etc/nginx/conf.d/openasp.conf`：

```nginx
server {
    listen 8080;
    server_name _;
    root /srv/www/example;
    index default.asp;

    client_max_body_size 32m;

    location / {
        try_files $uri $uri/ /default.asp?$query_string;
    }

    location ~ \.asp$ {
        try_files $uri =404;
        include /etc/nginx/fastcgi_params;

        fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
        fastcgi_param SCRIPT_NAME     $fastcgi_script_name;
        fastcgi_param DOCUMENT_ROOT   $document_root;
        fastcgi_param PATH_INFO       "";
        fastcgi_param PATH_TRANSLATED $document_root$fastcgi_script_name;

        fastcgi_pass 127.0.0.1:9000;
        fastcgi_read_timeout 120s;
    }

    location ~* \.(asa|inc)$ {
        deny all;
    }
}
```

Homebrew Nginx 的 include 通常位于
`/opt/homebrew/etc/nginx/fastcgi_params`。Nginx 的 `root` 必须和
`openasp-fpm --root` 指向同一目录；FastCGI 的 `SCRIPT_FILENAME` 是最终采用
的 ASP 物理路径。

检查配置、重载并访问页面：

```sh
sudo nginx -t
sudo nginx -s reload
curl 'http://127.0.0.1:8080/?name=OpenASP'
```

除非使用隔离的网关私有网络，否则应只让 FastCGI 监听回环地址。生产环境中应以
非特权账户运行所选 Web server 和 `openasp-fpm`，站点源码只授予读取权限，
仅允许写入明确的上传、状态、缓存和日志目录。普通站点不要开启 shell 与进程
执行权限。生产环境可使用 systemd 或 launchd 同时托管这两个进程。

### 2B. 配置 egret-web-server

[`egret-web-server`](https://github.com/egret-lang/egret-web-server) 是
Egret 原生实现的 Nginx 替代方案。它支持 Nginx 风格的 server/location 配置、
`try_files`、TCP 或 Unix socket `fastcgi_pass`、`fastcgi_param`、多进程
worker、AIO 连接、gzip、TLS 和反向代理。

使用 Egret 0.1.4 或更高版本编译：

```sh
git clone https://github.com/egret-lang/egret-web-server.git
cd egret-web-server
egret build
mkdir -p logs
```

创建 `conf/openasp.conf`：

```nginx
worker_processes 4;
error_log ./logs/error.log;

events {
    worker_connections 1024;
}

http {
    access_log ./logs/access.log;
    default_type application/octet-stream;
    keepalive_timeout 65;
    client_max_body_size 32m;

    server {
        listen 8080;
        server_name localhost;
        root /srv/www/example;
        index default.asp;

        location / {
            try_files $uri $uri/ /default.asp?$query_string;
        }

        location ~ \.asp$ {
            fastcgi_pass 127.0.0.1:9000;
            fastcgi_index default.asp;

            fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
            fastcgi_param SCRIPT_NAME     $fastcgi_script_name;
            fastcgi_param QUERY_STRING    $query_string;
            fastcgi_param REQUEST_METHOD  $request_method;
            fastcgi_param CONTENT_TYPE    $content_type;
            fastcgi_param CONTENT_LENGTH  $content_length;
            fastcgi_param REQUEST_URI     $request_uri;
            fastcgi_param DOCUMENT_ROOT   $document_root;
            fastcgi_param SERVER_PROTOCOL $server_protocol;
            fastcgi_param REMOTE_ADDR     $remote_addr;
            fastcgi_param REMOTE_PORT     $remote_port;
            fastcgi_param SERVER_ADDR     $server_addr;
            fastcgi_param SERVER_PORT     $server_port;
            fastcgi_param SERVER_NAME     $server_name;
            fastcgi_param HTTP_COOKIE     $http_cookie;
        }
    }
}
```

先按步骤 1 启动同一个 `openasp-fpm`，再启动 Web server：

```sh
./build/egret-web-server -c ./conf/openasp.conf --check-config
./build/egret-web-server -c ./conf/openasp.conf
curl 'http://127.0.0.1:8080/?name=OpenASP'
```

`openasp.conf` 中的 `root` 必须和 `openasp-fpm --root` 一致。网关本身会生成
基础 CGI/1.1 环境；这里显式列出参数是为了清楚展示 ASP 路径映射并保持配置可移植。
端口 9000 应只监听回环地址，并遵循上文相同的服务账户与文件系统权限限制。

## 核心能力

| 能力 | 实现 |
| --- | --- |
| VBScript | 词法、语法、类、数组、错误处理、`ByRef`、`On Error Resume Next` |
| ASP 运行时 | `Request`、`Response`、`Server`、`Session`、`Application` |
| 数据访问 | `OpenASP.MySQL`、`OpenASP.SQLite`、`OpenASP.MDB` |
| MDB / Jet 4 | 原生 DDL/DML、进程锁、Journal、崩溃恢复，无 Java 依赖 |
| 执行后端 | Egret VM、原生 C VM、AOT mmap 页缓存 |
| 服务模式 | 独立渲染、交互式 CLI、FastCGI 多进程服务 |
| 原生组件 | HTTP、WebSocket、Socket、Redis、Memcache、JSON、Crypto、OpenAI |
| 内存管理 | 请求 arena、GC 安全点、保守根校验与空闲压力回收 |

## Classic ASP 兼容能力

OpenASP 在 Unix 类系统上直接实现 Classic ASP 对象模型：

| 范围 | 已实现能力 |
| --- | --- |
| 内置对象 | `Request`、`Response`、`Server`、`Session`、`Application`、`ObjectContext` |
| 请求数据 | QueryString、Form、Cookie、ServerVariables、Header 和 `BinaryRead` |
| 响应控制 | 缓冲输出、Header、Cookie、状态码、重定向、文本与二进制写入 |
| Server 工具 | `CreateObject`、`MapPath`、`HTMLEncode`、`URLEncode`、脚本超时 |
| VBScript | 过程、类、数组、`ByRef`、错误处理、`On Error Resume Next` 和内置函数 |
| Include | Classic `<!--#include file=... -->` 与 virtual include |

Windows 平台常用 ProgID 由可移植运行时组件实现：

| 兼容 ProgID | 能力 |
| --- | --- |
| `Scripting.Dictionary` | 键值集合与默认 `Item` |
| `ADODB.Connection`、`Recordset`、`Command` | SQL、参数、游标、字段、分页和事务 |
| `ADODB.Stream` | 二进制/文本流、字符集转换和文件读写 |
| `Scripting.FileSystemObject` | 文件、目录、文本流和集合 |
| `VBScript.RegExp` | 基于 PCRE2 的匹配、捕获、替换和 SubMatches |
| `MSXML2.DOMDocument` 及别名 | XML 加载、解析、查询、修改和保存 |
| `MSXML2.ServerXMLHTTP`、`MSXML2.XMLHTTP` | HTTP/HTTPS 客户端兼容 |
| `Shell.Application` | 进程启动兼容；必须显式开启权限 |

ADO 通过连接字符串选择原生 Provider：

```asp
Set db = Server.CreateObject("ADODB.Connection")
db.Open "Provider=OpenASP.SQLite;Data Source=/srv/www/example/data/site.db"

Set rows = db.Execute("SELECT id, title FROM posts ORDER BY id DESC")
Do Until rows.EOF
    Response.Write Server.HTMLEncode(rows("title")) & "<br>"
    rows.MoveNext
Loop
```

| Provider | 连接字符串 |
| --- | --- |
| `OpenASP.MySQL` | `Provider=OpenASP.MySQL;Server=127.0.0.1;Port=3306;User Id=user;Password=secret;Database=app;Charset=utf8mb4` |
| `OpenASP.SQLite` | `Provider=OpenASP.SQLite;Data Source=/absolute/path/app.db` |
| `OpenASP.MDB` | `Provider=OpenASP.MDB;Data Source=/absolute/path/data.mdb` |

MDB/Jet 4 支持原生读取、DDL/DML、AutoNumber、多进程锁、Journal 与崩溃恢复，
不依赖 Java 或 Microsoft Access。

## OpenASP 扩展组件

扩展组件通过 `Server.CreateObject("OpenASP.Name")` 创建，ProgID 不区分大小写。
活动 socket 和连接归属当前请求上下文，请求结束时会确定性关闭。

| 组件 | 主要能力 |
| --- | --- |
| `OpenASP.JSON` | `Parse`、`Stringify`、`Validate`、`Minify`、`Escape`；解析对象提供 `Item`、`Exists`、`Count`、`Keys`、`Items` |
| `OpenASP.HttpClient` | `Get`、`Post`、`Send`、自定义 Header、响应读取、超时/正文限制、TLS 验证和私有 CA |
| `OpenASP.WebSocket` | `ws://`/`wss://` 客户端与服务端升级、文本/二进制帧、Ping/Pong、关闭信息与子协议 |
| `OpenASP.Socket` | TCP、UDP、Unix socket；连接/监听/接受、收发、数据报、地址、端口、状态和超时 |
| `OpenASP.Redis` | RESP 连接、认证、数据库选择、常用 string/hash/list/set 方法和任意 `Execute` 命令 |
| `OpenASP.Memcache` | text/meta/binary 协议、读取/存储/CAS/计数器/统计、可选 TLS 和私有 CA |
| `OpenASP.Crypto` | SHA-256、HMAC-SHA256、Base64/Base64URL、CSPRNG 随机字节/十六进制、常量时间比较 |
| `OpenASP.Process` | 有界同步执行、受管子进程生命周期，以及 standalone POSIX fork/wait/signal |
| `OpenASP.OpenAI` | OpenAI 兼容的 Chat Completions 与 Responses API，可配置模型、端点、超时和正文限制 |

### OpenASP.JSON

`OpenASP.JSON` 将 JSON 解析为 VBScript 可访问的对象和数组，并能重新序列化，
不需要业务代码引入脚本版 JSON 库。对象键区分大小写。网关只需校验或转发数据时，
可使用完全在原生层执行的 `Validate` 和 `Minify`，避免创建对象图。

```asp
<%
Response.ContentType = "application/json"
Set json = Server.CreateObject("OpenASP.JSON")

payload = "{""requestId"":""req-42"",""items"":[1,2,3]," & _
          """metadata"":{""source"":""classic-asp""}}"

If Not json.Validate(payload) Then
    Response.Status = "400 Bad Request"
    Response.Write "{""error"":""invalid JSON""}"
    Response.End
End If

Set document = json.Parse(payload)
Set metadata = document("metadata")
metadata("processed") = True

Response.Write json.Stringify(document)
%>
```

公开方法包括 `Parse`、`Stringify`、`Validate`、`Minify` 和 `Escape`。
解析后的对象提供默认 `Item`、`Exists`、`Count`、`Keys`、`Items`，数组使用
普通 VBScript 下标。原生校验器的最大嵌套深度为 128。

### OpenASP.HttpClient

`OpenASP.HttpClient` 是用于 REST API 和内部服务调用的同步 HTTP/HTTPS 客户端。
它强制使用绝对 URL，并提供响应大小限制、超时、Header CR/LF 防注入、证书链与
主机名校验。

```asp
<%
Set http = Server.CreateObject("OpenASP.HttpClient")
http.Timeout = 10000
http.MaxResponseBytes = 1048576
http.TLSVerify = True
http.CAFile = "/etc/ssl/private-service-ca.pem"

Call http.SetHeader("Accept", "application/json")
Call http.SetHeader("Content-Type", "application/json")
body = http.Post( _
    "https://service.example/v1/items", _
    "{""name"":""OpenASP"",""enabled"":true}" _
)

Response.ContentType = "application/json"
If http.Status >= 200 And http.Status < 300 Then
    Response.Write body
Else
    Response.Status = "502 Bad Gateway"
    Response.Write "{""upstreamStatus"":" & http.Status & "}"
End If

Call http.ClearHeaders()
%>
```

请求方法为 `Get(url)`、`Post(url, body)` 和 `Send(method, url, body)`。
通过 `Status`、`ResponseText`/`ResponseBody`、`ResponseHeaders` 和
`GetResponseHeader(name)` 读取结果。默认超时 30 秒，默认最大响应 16 MiB。

### OpenASP.WebSocket

`OpenASP.WebSocket` 既能作为 `ws://`、`wss://` 客户端，也能从
`OpenASP.Socket` 接受的连接升级为服务端 WebSocket。客户端帧自动 mask，
收到 Ping 时底层自动回复 Pong。

```asp
<%
Set ws = Server.CreateObject("OpenASP.WebSocket")
ws.Timeout = 15000
ws.TLSVerify = True
' ws.CAFile = "/etc/ssl/private-service-ca.pem"

Call ws.Connect("wss://service.example/events", "json")
Call ws.SendText("{""type"":""ping"",""id"":42}")
message = ws.Receive()

Response.ContentType = "application/json"
Set json = Server.CreateObject("OpenASP.JSON")
Response.Write "{""state"":" & ws.State & _
    ",""messageType"":""" & ws.MessageType & """" & _
    ",""payload"":""" & json.Escape(message) & """}"

Call ws.Close()
%>
```

完整帧接口包括 `SendText`、`SendBinary`、`Receive`/`Recv`、`Ping`、`Pong`
和 `Close`。状态信息包括 `State`、`MessageType`、`Opcode`、`Final`、
`CloseCode` 和 `Subprotocol`。服务端调用
`ws.Accept(acceptedSocket, subprotocol)`，成功后 socket 所有权转移给
WebSocket 对象。

### OpenASP.Socket

`OpenASP.Socket` 提供 TCP、UDP 和 Unix domain socket，既可作为客户端，也能
创建 TCP/Unix 监听器或 UDP 端点。下面的完整 TCP 客户端假设
`127.0.0.1:9001` 上运行着 echo 服务：

```asp
<%
Set socket = Server.CreateObject("OpenASP.Socket")
socket.Protocol = "tcp"
socket.Timeout = 5000

Call socket.Connect("127.0.0.1", 9001)
sent = socket.Send("hello from OpenASP")
reply = socket.Receive(4096)

Response.ContentType = "text/plain"
Response.Write "state=" & socket.State & vbCrLf
Response.Write "bytes-sent=" & sent & vbCrLf
Response.Write "reply=" & reply

Call socket.Close()
%>
```

TCP/Unix 服务端使用 `Listen` 和 `Accept`；UDP 使用 `Bind`、`SendTo` 和
`ReceiveFrom`，后者返回 `[data, peerHost, peerPort]`。配置和状态属性包括
`Protocol`、`Host`/`Address`/`Path`、`Port`、`Timeout`、`State`、
`LocalAddress`、`RemoteAddress` 和 `LastPeerAddress`。

### OpenASP.Redis

`OpenASP.Redis` 直接实现 RESP 协议，并保留 Null、integer、boolean 和 array
响应类型。它支持独立连接参数或连接字符串，请求上下文销毁时会关闭活动连接。

```asp
<%
Set redis = Server.CreateObject("OpenASP.Redis")
Call redis.Open( _
    "Server=127.0.0.1;Port=6379;Database=0;Timeout=5000" _
)

Call redis.SetEx("session:42", "active", 60)
Call redis.HSet("user:42", "name", "OpenASP")
Call redis.Del("jobs:pending")
Call redis.RPush("jobs:pending", "compile")
Call redis.RPush("jobs:pending", "deploy")

jobs = redis.LRange("jobs:pending", 0, -1)
Response.ContentType = "text/plain"
Response.Write redis.Get("session:42") & vbCrLf
Response.Write redis.HGet("user:42", "name") & vbCrLf
Response.Write Join(jobs, ",")

Call redis.Del("session:42", "user:42", "jobs:pending")
Call redis.Close()
%>
```

String 操作包括 `Get`、`Set`、`SetEx`、`Del`、`Exists`、`Expire`、`TTL`、
`Incr`、`Decr`；Hash 包括 `HGet`、`HSet`、`HGetAll`；List 包括 `LPush`、
`RPush`、`LPop`、`RPop`、`LRange`；Set 包括 `SAdd`、`SMembers`。
`Ping`、`Auth`、`Select` 用于连接管理，未封装的命令可通过
`Execute(command, ...)` 发送。

### OpenASP.Memcache

`OpenASP.Memcache` 支持 text、meta 和 binary 协议，以及 CAS、计数器、统计与
可选 TLS。Value 使用带长度的 String，因此不会截断其中的 `0x00`。

```asp
<%
Set cache = Server.CreateObject("OpenASP.Memcache")
cache.Host = "127.0.0.1"
cache.Port = 11211
cache.Protocol = "text"
cache.Timeout = 5000
Call cache.Open()

Call cache.Set("page:home", "<h1>OpenASP</h1>", 0, 60)
cached = cache.Get("page:home")

If IsNull(cached) Then
    Response.Status = "404 Not Found"
Else
    Response.Write cached
End If

Call cache.Delete("page:home")
Call cache.Close()
%>
```

存储操作包括 `Set`、`Add`、`Replace`、`Append`、`Prepend` 和 `CAS`；
修改操作包括 `Delete`、`Touch`、`Incr` 和 `Decr`。`Get`/`Gets`、
`LastFlags`、`LastCAS` 用于读取值和元数据；`Stats`、`Version`、
`FlushAll` 提供管理操作。

### OpenASP.Crypto

`OpenASP.Crypto` 基于 OpenSSL 实现哈希、签名、编码、安全随机数和常量时间比较。
二进制输入输出能够保留 `0x00`。

```asp
<%
Set crypto = Server.CreateObject("OpenASP.Crypto")

token = crypto.Base64URLEncode(crypto.RandomBytes(32))
signature = crypto.HMACSHA256("replace-with-server-secret", token)
expected = crypto.HMACSHA256("replace-with-server-secret", token)

Response.ContentType = "text/plain"
Response.Write "token=" & token & vbCrLf
Response.Write "sha256=" & crypto.SHA256("OpenASP") & vbCrLf
Response.Write "signature-valid=" & _
    CStr(crypto.ConstantTimeEquals(signature, expected))
%>
```

方法包括 `SHA256`、`HMACSHA256`、`Base64Encode`/`Base64Decode`、
`Base64URLEncode`/`Base64URLDecode`、`RandomBytes`、`RandomHex` 和
`ConstantTimeEquals`。SHA-256 与 HMAC 输出小写十六进制。

### OpenASP.Process

`OpenASP.Process` 直接执行指定程序，不会隐式经过 shell。执行时间和输出大小均
受限，超时会终止整个子进程组。该高权限组件默认关闭，只有可信应用才能在宿主
命令中加入 `--process-enable=on`。

```asp
<%
Set process = Server.CreateObject("OpenASP.Process")
process.Timeout = 5000
process.MaxOutputBytes = 65536

Set result = process.Run( _
    "/usr/bin/uname", "-a", "/", "", 5000, 65536 _
)

Response.ContentType = "text/plain"
Response.Write "exit=" & result.ExitCode & vbCrLf
Response.Write "timed-out=" & result.TimedOut & vbCrLf
Response.Write "stdout=" & result.StdOut & vbCrLf
Response.Write "stderr=" & result.StdErr
%>
```

`Run` 为同步执行。`Start`、`PID`、`IsRunning`、`Wait`、`Terminate` 管理
长期子进程。底层 `Fork`、`WaitPid`、`Signal`、`Exit` 只能用于 standalone
单线程启动阶段，禁止在 FPM 请求内使用。`OpenASP.Process` 与
`Shell.Application` 使用相互独立的权限开关。

### OpenASP.OpenAI

`OpenASP.OpenAI` 让 ASP 应用无需桥接进程即可调用 OpenAI 兼容模型服务。
`BaseURL` 可指向 OpenAI 或兼容的本地/私有服务。传入普通字符串时自动构造最小
请求；传入合法 JSON 时原样转发，可承载工具调用、多模态输入、结构化输出或
供应商扩展字段。

```asp
<%
Set ai = Server.CreateObject("OpenASP.OpenAI")
ai.APIKey = Application("OPENAI_API_KEY")
ai.BaseURL = "https://api.openai.com/v1"
ai.Model = "gpt-4o-mini"
ai.Timeout = 120000
ai.MaxResponseBytes = 16777216

raw = ai.Responses("请总结这个 Classic ASP 应用。")

Response.ContentType = "application/json"
If ai.Status >= 200 And ai.Status < 300 Then
    Set json = Server.CreateObject("OpenASP.JSON")
    Set result = json.Parse(raw)
    Response.Write json.Stringify(result)
Else
    Response.Status = "502 Bad Gateway"
    Response.Write "{""modelServiceStatus"":" & ai.Status & "}"
End If
%>
```

`ChatCompletions(promptOrJson)` 请求 `/chat/completions`，
`Responses(inputOrJson)` 请求 `/responses`；`Status` 和 `LastResponse`
保存最近一次调用。API Key 必须由部署密钥机制加载，严禁写入源码、查询字符串或
响应正文；OpenASP 不会将它写入请求日志。

准确参数签名、上限、TLS 行为和生命周期规则详见
[OpenASP 后端组件](docs/OpenASP后端组件.md)。

## 架构

![OpenASP 运行时架构](assets/openasp-architecture.svg)

请求可以来自 standalone 执行、保留状态的 CLI、嵌入式宿主，或者 Nginx 后方的
`openasp-fpm` worker 池。ASP 模板和 include 经 VBScript 前端编译为缓存字节码
或 mmap AOT 页面镜像。Egret VM 与原生 C VM 共用同一套 ASP 对象模型、ADO
Provider、扩展组件注册表、请求状态、日志、arena 和 GC 服务。

代码按职责分布：

| 目录 | 职责 |
| --- | --- |
| `src/asp/compiler/` | ASP 模板预处理与 VBScript 前端 |
| `src/asp/vm/` | 字节码、解释执行和 VM 调度 |
| `src/asp/runtime/` | Variant、请求状态、缓存和序列化 |
| `src/asp/object/` | ASP、ADO 和后端组件对象模型 |
| `src/asp/server/` | FastCGI 协议、进程模型和请求映射 |
| `native/` | 数据库、GC、C VM、系统与文本原生实现 |
| `fixtures/pages/` | 语义与组件回归页面 |
| `tests/` | C 层、并发和集成测试 |

## 验证

```sh
make test-source-paths
make test-openasp-commands
make test-vbscript-bnf
make test-backend-components
make test-mdb-native
```

原生 C 模块使用 `-Wall -Wextra -Werror` 构建。MDB 回归覆盖 DDL/DML、
多进程并发、AutoNumber 唯一性、强制崩溃恢复和数据一致性。

## 文档

- [OpenASP 命令行](docs/OpenASP命令行.md)
- [OpenASP 后端组件](docs/OpenASP后端组件.md)
- [VBScript BNF 支持矩阵](docs/VBSCRIPT_BNF_SUPPORT.md)
- [Classic ASP 可行性与设计](docs/classic-asp-feasibility-and-design.md)
- [AOT 内存优化](docs/AOT_MEMORY_OPTIMIZATION.md)
- [Z-Blog 本地运行](docs/ZBlog本地运行.md)
- [Native 模块说明](native/README.md)
