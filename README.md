# genda

Personal agenda server: classifies emails + phone notifications into
calendar events. Never miss an appointment or obligation.

Single C file (`genda.c`), SQLite, MessagePack wire format.

## Build

```sh
make        # builds ./genda
```

## Run

```sh
GENDA_PORT=8080 GENDA_DB=./genda.db GENDA_TOKEN=secret ./genda
```
