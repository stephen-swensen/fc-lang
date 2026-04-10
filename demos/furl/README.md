# furl

An HTTP client (curl clone) written in FC, demonstrating TCP client networking with the `std::net` stdlib module.

Supports HTTP/1.0 GET, HEAD, POST, custom headers, verbose output, and file output. HTTPS is not supported (no TLS).

Runs on Linux and on Windows under MSYS2 / MinGW (UCRT64 or MINGW64). The `run.sh` auto-detects the host and links Winsock (`-lws2_32`) on Windows.

## Usage

```
./demos/furl/run.sh [options] <url>
```

Options:

| Flag | Description |
|------|-------------|
| `-v` | Verbose — show request/response headers on stderr |
| `-I` | HEAD request — print response headers only |
| `-X METHOD` | HTTP method (GET, POST, PUT, DELETE, ...) |
| `-H "Header: value"` | Add a custom header (repeatable, up to 16) |
| `-d DATA` | Request body (auto-selects POST if method is GET) |
| `-o FILE` | Write response body to file instead of stdout |

## Examples

Basic GET:

```
$ ./demos/furl/run.sh http://example.com
<!doctype html><html lang="en">...
```

Verbose (request `>` and response `<` headers on stderr):

```
$ ./demos/furl/run.sh -v http://example.com
> GET / HTTP/1.0
> Host: example.com
> User-Agent: furl/1.0
> Accept: */*
> Connection: close
>
< HTTP/1.1 200 OK
< Content-Type: text/html
< Connection: close
<
<!doctype html><html lang="en">...
```

HEAD request (headers only):

```
$ ./demos/furl/run.sh -I http://example.com
HTTP/1.1 200 OK
Content-Type: text/html
Connection: close
Server: cloudflare
```

POST with data:

```
$ ./demos/furl/run.sh -d "hello=world" http://httpbin.org/post
{
  "data": "hello=world",
  "headers": {
    "Content-Length": "11",
    "Host": "httpbin.org",
    "User-Agent": "furl/1.0"
  },
  ...
}
```

Custom headers and output to file:

```
$ ./demos/furl/run.sh -H "Accept: text/html" -o /tmp/page.html http://example.com
$ cat /tmp/page.html
<!doctype html>...
```

## FC features demonstrated

- **TCP client networking** via `std::net`: Uses `open_tcp`, `connect_addr`, `send_bytes`, and `recv_bytes` for a full HTTP request/response cycle.
- **DNS resolution** via `net.resolve`: Hostname-to-IP lookup using the stdlib's `std::net` module (which wraps `gethostbyname` and `inet_addr` internally).
- **String parsing with `std::text`**: URL parsing uses `starts_with`, `index_of`, and `parse_int32` to extract host, port, and path components via subslicing (zero-copy).
- **Heap-allocated interpolated strings**: HTTP request lines are built dynamically with `alloc("%s{method} %s{path} HTTP/1.0\r\n...")!` and freed after sending.
- **Option types for error handling**: Socket creation (`int32?`), DNS resolution (`uint32?`), file open (`any*?`), and string search (`int64?`) all use option types with `.is_none` checks and `!` unwrap.
- **Streaming I/O**: Response is read in 4KB chunks and streamed directly to output, with header accumulation limited to 8KB — handles responses of any size.
- **File I/O with `std::io`**: Supports `-o` flag via `io.open`/`io.write`/`io.close` for writing response body to a file.
- **String interpolation**: Formatted output with `%s{}` and `%d{}` for verbose headers, error messages, and dynamic request construction.
- **Stdlib modules**: Uses four stdlib modules together — `std::net` (TCP sockets), `std::io` (file I/O), `std::sys` (exit), and `std::text` (string parsing).
