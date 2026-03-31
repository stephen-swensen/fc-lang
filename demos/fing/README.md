# fing

A ping clone written in FC, demonstrating network programming with the `std::net` stdlib module.

## Usage

```
./demos/fing/run.sh <host>
```

The first run will prompt for `sudo` to set `cap_net_raw` on the binary (same capability `/usr/bin/ping` uses for raw ICMP sockets). After that, subsequent rebuilds will re-prompt since the binary is recompiled.

Examples:

```
./demos/fing/run.sh google.com
./demos/fing/run.sh 8.8.8.8
```

Output:

```
FING google.com (142.250.217.142)
64 bytes from 142.250.217.142: icmp_seq=0 ttl=118 time=17.3 ms
64 bytes from 142.250.217.142: icmp_seq=1 ttl=118 time=18.1 ms
64 bytes from 142.250.217.142: icmp_seq=2 ttl=118 time=16.9 ms
64 bytes from 142.250.217.142: icmp_seq=3 ttl=118 time=17.0 ms

--- google.com fing statistics ---
4 packets transmitted, 4 received, 0% packet loss
rtt min/avg/max = 16.9/17.3/18.1 ms
```

## FC features demonstrated

- **C interop via `extern`**: Binds to POSIX socket APIs (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`), timing (`sys/time.h`), and DNS (`netdb.h`) using `extern` functions, structs, and constants.
- **Extern constants with `as` aliasing**: `extern SOCK_RAW as sock_raw: int32` pulls C macro values from headers with FC-style naming.
- **Extern structs**: `struct timeval`, `struct hostent`, and `struct sockaddr_in` are declared as extern structs and used directly.
- **Pointer casts and dereferencing**: DNS resolution casts `any*` to `uint32**` and double-dereferences to extract the resolved IP from `hostent.h_addr_list`.
- **Option types for error handling**: `open_icmp()` returns `int32?`, checked with `.is_none` / unwrap `!`.
- **Bitwise operations**: ICMP checksum computed in pure FC using `<<`, `>>`, `|`, `&`, `~` on `uint16`/`uint32`.
- **Raw byte buffer manipulation**: ICMP packets constructed by writing individual bytes into `uint8[]` slices via `.ptr[i]` indexing.
- **String interpolation**: `io.write` with `%d{expr}` and `%s{expr}` for formatted output.
- **Stdlib modules**: Uses `std::net` (sockets, byte order, address construction), `std::sys` (PID, sleep, exit), and `std::io` (file handle output).
