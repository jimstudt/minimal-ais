# minimal-ais

Tiny POSIX/C AIS tools.

Build:

```sh
make
```

Run the serial-to-UDP forwarder:

```sh
./minimal-ais-serial/minimal-ais-serial -s /dev/ttyUSB0 -b 38400 -d 127.0.0.1:10110 -d 192.0.2.50:10110:30s -v
```

Serial options:

- `-s serial`: serial device path.
- `-b baud`: serial baud rate. Defaults to `38400`.
- `-d host:port`: UDP destination. Repeat for multiple destinations.
- `-d host:port:30s`: rate limit that destination to one broadcast per MMSI every 30 seconds.
- `-v`: print each valid AIS line before forwarding it.

Run the UDP-to-JSON scoreboard:

```sh
./minimal-ais-json/minimal-ais-json -l 0.0.0.0:10110 -o ais.json
```

JSON options:

- `-l address:port`: UDP address and port to listen on.
- `-o file.json`: JSON output file.
- `-v`: print each valid AIS line before processing it.

The validator is intentionally shallow: it accepts NMEA/AIS lines that start
with `!` and whose checksum after `*` matches the XOR of the payload bytes.

Rate limiting decodes only enough AIS payload to find the MMSI. Lines whose
MMSI cannot be decoded are forwarded normally.

The JSON scoreboard processes AIS message types `1`, `2`, `3`, `4`, `18`,
`21`, and `24`. It tracks sender type, name, location, heading, speed, MMSI,
and update time by MMSI. The file is rewritten when a processed message changes
the scoreboard, and at least once a minute while the program is running.
