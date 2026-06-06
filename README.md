# minimal-ais

`minimal-ais` is a pair of tiny POSIX/C programs for moving AIS data around.
The main tool reads NMEA AIS sentences from a serial receiver and forwards them
to one or more UDP destinations. That is the common use case: take the AIS feed
from a local radio or receiver and forward it to local tools, another machine,
or AIS clearing houses.

The second tool is optional. It listens for UDP AIS messages and writes a small
JSON scoreboard of recently seen MMSIs. Use it if you want a local, easy-to-read
snapshot of nearby vessels or stations.

The programs are intentionally small and unsurprising. They use ordinary C,
POSIX sockets, a bare Makefile, fixed-size tables, and no third-party runtime
dependencies. They are meant to run quietly on small Debian machines with very
low CPU and memory use.

## Components

- `minimal-ais-serial`: reads AIS messages from a serial port, validates the
  NMEA checksum, and forwards valid lines to UDP destinations.
- `minimal-ais-json`: listens for UDP AIS messages and writes a JSON file sorted
  by MMSI.

You can use only `minimal-ais-serial`. The JSON component is not required for
forwarding AIS data.

## Build

```sh
make
```

This builds:

- `minimal-ais-serial/minimal-ais-serial`
- `minimal-ais-json/minimal-ais-json`

## Install

```sh
make install
```

The install target honors `DESTDIR`, `PREFIX`, `BINDIR`, and `MANDIR`.
By default it installs executables under `/usr/local/bin` and man pages under
`/usr/local/share/man/man1`.

## Debian Package

```sh
make deb
```

The Debian package installs the programs under `/usr/bin`, man pages under
`/usr/share/man/man1`, and systemd unit files under `/etc/systemd/system`.
The included services use `/dev/ttyACM1`, `127.0.0.1:10110`, and
`/run/minimal-ais/current.json`. systemd creates `/run/minimal-ais` for the
JSON service.

After installing the package, run:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now minimal-ais-json.service
sudo systemctl enable --now minimal-ais-serial.service
```

Enable only the services you want. For forwarding-only use, enable
`minimal-ais-serial.service` and skip `minimal-ais-json.service`.

## Configuration

If you run the programs under systemd, edit the service files installed in
`/etc/systemd/system`.

For `minimal-ais-serial.service`, set your serial device and UDP destinations:

```ini
ExecStart=/usr/bin/minimal-ais-serial -s /dev/ttyACM1 -b 38400 -d 127.0.0.1:10110
```

You can forward to multiple destinations by repeating `-d`:

```ini
ExecStart=/usr/bin/minimal-ais-serial -s /dev/ttyACM1 -b 38400 \
  -d 127.0.0.1:10110 \
  -d ais.example.net:10110 \
  -d 10.10.10.255:10110:30s
```

The optional `:30s` suffix rate limits that destination to one forwarded
sentence per decoded MMSI every 30 seconds. This can be useful when a consumer
does not want every repeated position report. Lines whose MMSI cannot be
decoded are forwarded normally.

For `minimal-ais-json.service`, set the listen address and output file if the
defaults do not fit:

```ini
ExecStart=/usr/bin/minimal-ais-json -l 127.0.0.1:10110 -o /run/minimal-ais/current.json -a 12h
```

After changing a service file:

```sh
sudo systemctl daemon-reload
sudo systemctl restart minimal-ais-serial.service
sudo systemctl restart minimal-ais-json.service
```

Restart only the service you changed.

## Serial Forwarder

Example:

```sh
./minimal-ais-serial/minimal-ais-serial \
  -s /dev/ttyUSB0 \
  -b 38400 \
  -d 127.0.0.1:10110 \
  -d 192.0.2.50:10110:30s \
  -v
```

Options:

- `-s serial`: serial device path.
- `-b baud`: serial baud rate. Defaults to `38400`.
- `-d host:port`: UDP destination. Repeat for multiple destinations.
- `-d host:port:30s`: rate limit that destination to one broadcast per MMSI
  every 30 seconds.
- `-v`: print each valid AIS line before forwarding it.

The validator is intentionally shallow: it accepts NMEA/AIS lines that start
with `!` and whose checksum after `*` matches the XOR of the payload bytes.

## JSON Scoreboard

Example:

```sh
./minimal-ais-json/minimal-ais-json -l 0.0.0.0:10110 -o ais.json -a 12h
```

Options:

- `-l address:port`: UDP address and port to listen on.
- `-o file.json`: JSON output file.
- `-a duration`: drop entries not seen within this interval. Defaults to `12h`.
  Durations may be bare seconds or use `s`, `m`, `h`, or `d`.
- `-v`: print each valid AIS line before processing it.

The JSON scoreboard processes AIS message types `1`, `2`, `3`, `4`, `18`,
`21`, and `24`. It tracks sender type, name, location, heading, speed, MMSI,
update time, and last-seen time by MMSI. The file is rewritten when a processed
message changes the scoreboard, and at least once a minute while the program is
running.
