# genda

Personal agenda server: classifies emails + phone notifications into
calendar events. Never miss an appointment or obligation.

Single C file (`genda.c`), SQLite, MessagePack wire format. No JSON on
the wire.

## Build & test

```sh
make        # builds ./genda
make test   # builds + runs greatest.h integration tests
```

Deps: system `sqlite3` + `libcurl` + `pthread`. One vendored header:
`vendor/greatest.h` (tests only).

## Run

```sh
GENDA_PORT=8080 GENDA_DB=./genda.db GENDA_TOKEN=secret ./genda
```

Listens on loopback; put Caddy/Tailscale in front for TLS.

| Env                 | Default      | Purpose                                  |
| ------------------- | ------------ | ---------------------------------------- |
| `GENDA_PORT`        | `8080`       | listen port                              |
| `GENDA_DB`          | `./genda.db` | sqlite file (created if missing)         |
| `GENDA_TOKEN`       | empty (open) | Bearer token; enforced when set          |
| `GENDA_IMAP_URL`    | empty (off)  | e.g. `imaps://mail.example.com/INBOX`    |
| `GENDA_IMAP_USER`   | empty        | IMAP username                            |
| `GENDA_IMAP_PASS`   | empty        | IMAP password                            |
| `GENDA_IMAP_POLL_SEC` | `300`      | poll interval (min 60)                   |

## API

All bodies are `application/msgpack`. Auth: `Authorization: Bearer $TOKEN`.

- `GET /health` → `ok` (no auth)
- `POST /ingest` → map `{source,app,title,text,time,from,ext_id}`
  (missing `ext_id` is derived by hash). Replies `{raw_id,is_event}`.
  Re-ingest of the same `ext_id` returns the same `raw_id` and never
  duplicates the event.
- `GET /events?since=&until=` → array of
  `{id,raw_id,title,starts_at,deadline,location,kind,confidence,created_at}`.
  Range compares `starts_at`, falling back to `deadline`, then `created_at`.

`kind` is `appointment`, `obligation`, or `none`. Recall-first: anything
with `confidence >= 0.3` is stored; clients should highlight `>= 0.7`.

## Status

- Classifier is currently a keyword heuristic mock
  (see `classify_input` TODO). Real LLM integration lands later.
- IMAP poller marks nothing read; it tracks `imap_last_uid` in `meta`
  and skips already-seen UIDs.
- Future app: Kotlin Multiplatform against `GET /events`; website stays
  a simple page served separately.
