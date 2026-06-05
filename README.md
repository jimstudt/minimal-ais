# minimal-ais

Tiny POSIX/C AIS serial-to-UDP forwarder.

Build:

```sh
make
```

Run:

```sh
./minimal-ais -s /dev/ttyUSB0 -b 38400 -d 127.0.0.1:10110 -d 192.0.2.50:10110 -v
```

Options:

- `-s serial`: serial device path.
- `-b baud`: serial baud rate. Defaults to `38400`.
- `-d host:port`: UDP destination. Repeat for multiple destinations.
- `-v`: print each valid AIS line before forwarding it.

The validator is intentionally shallow: it accepts NMEA/AIS lines that start
with `!` and whose checksum after `*` matches the XOR of the payload bytes.
