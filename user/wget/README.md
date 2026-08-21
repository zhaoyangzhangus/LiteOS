# LiteOS wget

The bundled client supports HTTP/1.1 downloads over the LiteOS IPv4 socket
API. It handles DNS A lookup, redirects (including relative paths), chunked
and fixed-length responses, `-O`, `-P`, `-c`, `-nc`, retries, timeouts,
custom headers, Basic authentication, POST data, user-agent and referer.
HTTPS uses the vendored BearSSL TLS 1.2 client and can be used with
the bundled ISRG Root X1 anchor; the certificate date is derived from the
build timestamp because the current image has no wall-clock syscall.
FTP supports anonymous/password login, passive-mode `RETR`, `-c`, `-nc`,
timeouts and output selection.

Examples:

```text
wget http://10.0.2.2:8000/file.txt
wget -O /tmp/file.txt http://10.0.2.2:8000/file.txt
wget -c -t 5 --timeout 15 http://10.0.2.2:8000/file.txt
wget --user alice --password secret --header "X-Trace: 1" URL
wget --post-data "name=liteos" --method POST URL
wget --no-check-certificate https://example.invalid/file.txt
wget --user user --password secret ftp://example.invalid/file.txt
```

Certificate validation is fail-closed when the bundled trust anchor or
build-time validation date does not match; `--no-check-certificate` deliberately
disables that check for controlled/test endpoints. FTP currently implements
passive binary downloads, not uploads, active mode, or recursive mirroring.
