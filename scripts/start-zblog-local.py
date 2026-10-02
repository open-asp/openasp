# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Start the existing persistent database and local Z-Blog services."""

import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MYSQL_DIR = ROOT / ".run/mysql"
DATA_DIR = MYSQL_DIR / "data"
NGINX_BIN = Path(os.environ.get("OPENASP_NGINX_BIN") or shutil.which("nginx") or "nginx")
NGINX_CONFIG = ROOT / ".run/nginx-zblog-mysql.conf"
ZBLOG_SOURCE = Path(os.environ.get("OPENASP_ZBLOG_SOURCE", ROOT.parent / "zblogasp-master"))


def mysql_layout():
    """Resolve the MySQL executable and installation root from configuration or PATH."""
    configured_base = os.environ.get("OPENASP_MYSQL_BASE")
    configured_binary = os.environ.get("OPENASP_MYSQLD")
    binary_name = configured_binary or shutil.which("mysqld")
    if configured_base:
        base = Path(configured_base).expanduser().resolve()
        binary = Path(binary_name).expanduser().resolve() if configured_binary else base / "bin/mysqld"
    elif binary_name:
        binary = Path(binary_name).expanduser().resolve()
        base = binary.parent.parent
    else:
        raise SystemExit(
            "mysqld was not found; add it to PATH or set "
            "OPENASP_MYSQL_BASE/OPENASP_MYSQLD."
        )
    if not binary.is_file():
        raise SystemExit(f"mysqld does not exist: {binary}")
    return base, binary


def nginx_config_directory():
    """Resolve the active Nginx configuration directory without a fixed prefix."""
    configured = os.environ.get("OPENASP_NGINX_CONFIG_DIR")
    if configured:
        return Path(configured).expanduser().resolve()
    result = subprocess.run(
        [str(NGINX_BIN), "-V"], capture_output=True, text=True, check=False
    )
    match = re.search(r"(?:^|\s)--conf-path=(\"[^\"]+\"|'[^']+'|\S+)", result.stderr)
    if not match:
        raise SystemExit(
            "Cannot determine the Nginx configuration directory; "
            "set OPENASP_NGINX_CONFIG_DIR."
        )
    return Path(match.group(1).strip("\"'")).expanduser().resolve().parent


def listening(port):
    """Return whether a TCP listener already owns the requested local port."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3):
            return True
    except OSError:
        return False


def wait_ready(port, process, log):
    """Wait for readiness, surfacing early process exit with captured logs."""
    # Rebuilding the 19-page AOT image after a cache format change can take
    # several minutes. Report progress while the child is still healthy.
    deadline = time.monotonic() + (600 if port == 19139 else 180)
    next_notice = time.monotonic() + 30
    while time.monotonic() < deadline:
        if listening(port):
            return
        if process.poll() is not None:
            raise SystemExit(f"Service startup failed; inspect the log: {log}")
        if time.monotonic() >= next_notice:
            print(
                f"Waiting for port {port}; initial AOT compilation may be slow. "
                f"Log: {log}",
                flush=True,
            )
            next_notice = time.monotonic() + 30
        time.sleep(0.2)
    raise SystemExit(f"Timed out waiting for port {port}; inspect the log: {log}")


def start(args, port, log, environment=None):
    """Start one local service only when its port is currently unused."""
    if listening(port):
        print(f"Port {port} is already active")
        return
    with log.open("ab") as output:
        process = subprocess.Popen(args, cwd=ROOT, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
    wait_ready(port, process, log)
    print(f"Service started: port {port}, PID {process.pid}")


def prepare_site_root(site_root):
    """Restore the disposable site copy after build cleanup and reapply patches."""
    required = (site_root / "default.asp", site_root / "zb_system/login.asp",
                site_root / "zb_system/cmd.asp")
    restored = not all(path.is_file() for path in required)
    if restored:
        source_required = (ZBLOG_SOURCE / "default.asp",
                           ZBLOG_SOURCE / "zb_system/login.asp",
                           ZBLOG_SOURCE / "zb_system/cmd.asp")
        if not all(path.is_file() for path in source_required):
            raise SystemExit(
                f"The Z-Blog runtime directory is missing and cannot be restored "
                f"from {ZBLOG_SOURCE}. Set OPENASP_ZBLOG_SOURCE to the source directory."
            )
        shutil.copytree(
            ZBLOG_SOURCE,
            site_root,
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns(".git", ".DS_Store", "build"),
        )
    provider_path = site_root / "zb_system/FUNCTION/c_system_base.asp"
    provider = provider_path.read_bytes()
    legacy = b"Provider=Egret.MySQL;"
    current = b"Provider=OpenASP.MySQL;"
    if legacy in provider:
        provider_path.write_bytes(provider.replace(legacy, current))
    elif current not in provider:
        raise SystemExit(f"Cannot migrate the Z-Blog MySQL provider: {provider_path}")
    obsolete_installer = site_root / "mssql.asp"
    if obsolete_installer.exists():
        obsolete_installer.unlink()
    subprocess.run([
        "python3", str(ROOT / "scripts/apply-zblog-performance-patches.py"),
        str(site_root),
    ], cwd=ROOT, check=True)
    if restored:
        print(f"Restored the Z-Blog runtime directory: {site_root}")


def rebuild_zblog_caches(site_root):
    """Rebuild generated includes and pages through Z-Blog's own cache APIs."""
    include_dir = site_root / "zb_users/include"
    required = (
        include_dir / "navbar.asp",
        include_dir / "controlpanel.asp",
        include_dir / "searchpanel.asp",
        site_root / "zb_users/cache/sidebar.asp",
        site_root / "zb_users/cache/default.asp",
    )
    generated_pages = (required[-1],)
    unresolved = any(
        path.is_file()
        and "<#CACHE_INCLUDE_" in path.read_text(encoding="utf-8-sig")
        for path in generated_pages
    )
    if all(path.is_file() and path.stat().st_size > 0 for path in required) and not unresolved:
        return

    include_dir.mkdir(parents=True, exist_ok=True)
    maintenance = site_root / ".openasp-rebuild-cache.asp"
    maintenance.write_text("""<%@ CODEPAGE=65001 %>
<% Option Explicit %>
<% Response.Buffer=True %>
<!-- #include file="zb_users/c_option.asp" -->
<!-- #include file="zb_system/function/c_function.asp" -->
<!-- #include file="zb_system/function/c_system_lib.asp" -->
<!-- #include file="zb_system/function/c_system_base.asp" -->
<!-- #include file="zb_system/function/c_system_event.asp" -->
<!-- #include file="zb_system/function/c_system_plugin.asp" -->
<!-- #include file="zb_users/plugin/p_config.asp" -->
<%
Call System_Initialize()
Call BlogReBuild_Functions()
Call ClearGlobeCache()
Call LoadGlobeCache()
Call BlogReBuild_Default()
Call ExportRSS()
Call System_Terminate()
Response.Write "OPENASP_CACHE_REBUILD_OK"
%>
""", encoding="utf-8")
    try:
        with urllib.request.urlopen(
            "http://127.0.0.1:18088/.openasp-rebuild-cache.asp",
            timeout=300,
        ) as response:
            output = response.read().decode("utf-8", "replace")
    finally:
        maintenance.unlink(missing_ok=True)
    if "OPENASP_CACHE_REBUILD_OK" not in output:
        raise SystemExit(f"Z-Blog cache rebuild failed: {output.strip() or 'unknown error'}")
    invalid = [
        path for path in required
        if not path.is_file() or path.stat().st_size == 0
    ]
    unresolved = [
        path for path in generated_pages
        if path.is_file()
        and "<#CACHE_INCLUDE_" in path.read_text(encoding="utf-8-sig")
    ]
    if invalid or unresolved:
        names = ", ".join(str(path) for path in invalid + unresolved)
        raise SystemExit(f"Z-Blog cache rebuild is incomplete: {names}")
    print("Rebuilt Z-Blog page and sidebar caches")


def write_nginx_config(site_root):
    """Generate the local proxy config, including canonical Z-Blog routes."""
    nginx_config_dir = nginx_config_directory()
    mime_types = nginx_config_dir / "mime.types"
    fastcgi_params = nginx_config_dir / "fastcgi_params"
    if not mime_types.is_file() or not fastcgi_params.is_file():
        raise SystemExit(
            f"Nginx configuration directory lacks mime.types or fastcgi_params: "
            f"{nginx_config_dir}"
        )
    config = f"""worker_processes 1;
error_log "{ROOT / '.run/nginx-zblog-mysql-error.log'}" info;
pid "{ROOT / '.run/nginx-zblog-mysql.pid'}";

events {{
    worker_connections 1024;
}}

http {{
    include "{mime_types}";
    default_type application/octet-stream;
    access_log "{ROOT / '.run/nginx-zblog-mysql-access.log'}";
    sendfile on;
    client_max_body_size 34m;

    server {{
        listen 18088;
        server_name 127.0.0.1 localhost;
        root "{site_root}";
        index default.asp;

        # Canonicalize historical root entry points before FastCGI so relative
        # redirects and assets continue to resolve below /zb_system/.
        location = /cmd.asp {{
            return 307 /zb_system/cmd.asp$is_args$args;
        }}

        location = /login.asp {{
            return 307 /zb_system/login.asp$is_args$args;
        }}

        location / {{
            try_files $uri $uri/ /default.asp?$query_string;
        }}

        location ~ \\.asp$ {{
            try_files $uri =404;
            include "{fastcgi_params}";
            add_header Cache-Control "no-cache, must-revalidate" always;
            add_header Pragma "no-cache" always;
            add_header Expires "0" always;
            fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
            fastcgi_param SCRIPT_NAME $fastcgi_script_name;
            fastcgi_param DOCUMENT_ROOT $document_root;
            fastcgi_param PATH_INFO "";
            fastcgi_param PATH_TRANSLATED $document_root$fastcgi_script_name;
            fastcgi_pass 127.0.0.1:19139;
            fastcgi_read_timeout 600s;
        }}
    }}
}}
"""
    NGINX_CONFIG.write_text(config, encoding="utf-8")
    return NGINX_CONFIG


def reload_nginx():
    """Reload this site's Nginx master even when its stale PID file is empty."""
    result = subprocess.run([
        "pgrep", "-f",
        f"nginx: master process {NGINX_BIN} -c {NGINX_CONFIG}",
    ], capture_output=True, text=True, check=False)
    pids = [int(value) for value in result.stdout.split() if value.isdigit()]
    if len(pids) != 1:
        raise SystemExit(
            f"Cannot determine the Nginx master PID; inspect: {NGINX_CONFIG}"
        )
    os.kill(pids[0], signal.SIGHUP)
    print(f"Reloaded the Nginx configuration: PID {pids[0]}")


def main():
    """Launch the database and OpenASP services needed by the local Z-Blog."""
    mysql_base, mysqld = mysql_layout()
    site_root = ROOT / "examples/zblog-mysql-openasp"
    prepare_site_root(site_root)
    if not (DATA_DIR / "mysql").is_dir() or not (DATA_DIR / "zblogasp_mysql").is_dir():
        raise SystemExit(
            f"No existing Z-Blog database was found at {DATA_DIR}. Restore a "
            "backup first; this startup script does not create an empty database."
        )
    start([
        str(mysqld), "--no-defaults",
        f"--basedir={mysql_base}", f"--datadir={DATA_DIR}",
        "--port=13316", "--bind-address=127.0.0.1",
        f"--socket={MYSQL_DIR / 'mysql.sock'}", f"--pid-file={MYSQL_DIR / 'mysql.pid'}",
        f"--log-error={MYSQL_DIR / 'mysql.err'}",
        f"--lc-messages-dir={mysql_base / 'share/mysql'}",
        "--explicit-defaults-for-timestamp",
    ], 13316, MYSQL_DIR / "launcher.log")
    environment = dict(os.environ)
    # Keep runtime state independent of caches made against a different database.
    environment["EGRET_ASP_STATE_DIR"] = str(ROOT / ".run/zblog-state")
    # Request rendering is already GC-quiescent. Keep the tracing collector
    # available only for idle memory-pressure fallback until every generated
    # String temporary is request-arena owned.
    environment["EG_GC_DISABLE"] = "0"
    environment["EGRET_ASP_REQUEST_END_GC"] = "0"
    # Preserve the entry filter recorded by the previous 19-page AOT image.
    # A bare default.asp filter would also compile old installers and exports.
    environment.setdefault("EGRET_ASP_AOT_ENTRY_PAGES",
                           "view.asp,tags.asp,admin.asp,edit_article.asp,"
                           "edit_setting.asp,cmd.asp,c_html_js.asp,c_html_js_add.asp,"
                           "c_validcode.asp,style.css.asp")
    start([
        str(ROOT / "build/openasp-fpm"),
        "--root", str(site_root), "--host", "127.0.0.1",
        "--port", "19139", "--max-accepts", "1000",
        "--workers", "4", "--worker-connections", "4",
    ], 19139, ROOT / ".run/zblog-persistent-fastcgi.log", environment)
    nginx_config = write_nginx_config(site_root)
    nginx = [str(NGINX_BIN), "-c", str(nginx_config)]
    subprocess.run(nginx + ["-t"], check=True)
    if listening(18088):
        reload_nginx()
    else:
        subprocess.run(nginx, check=True)
    if not listening(18088):
        raise SystemExit(
            "Nginx is not listening on port 18088; inspect "
            ".run/nginx-zblog-mysql-error.log"
        )
    rebuild_zblog_caches(site_root)
    print(
        "Site started: http://127.0.0.1:18088/ "
        "(AOT enabled, 4 workers x 4 connections)"
    )


if __name__ == "__main__":
    main()
