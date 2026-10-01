# genda

Personal agenda server: classifies emails + phone notifications into
calendar events. Never miss an appointment or obligation.

Small C modules under `src/` (`main/server/db/classify/imap/msgpack`), SQLite,
MessagePack wire format. No JSON on the client wire (JSON is only spoken
toward OpenCode Zen, for classification).

## Build & test

```sh
make        # builds ./genda
make test   # builds + runs greatest.h integration tests
```

Deps: system `sqlite3` + `libcurl` + `pthread`. Vendored headers:
`src/vendor/jsmn.h` (JSON tokenizer for Zen replies) and
`test/vendor/greatest.h` (tests only). Tests talk to a fake Zen server
(`test/integration/zen_fake.h`), never the real one.

## Run

```sh
OPENCODE_API_KEY=... GENDA_PORT=8080 GENDA_DB=./genda.db GENDA_TOKEN=secret ./genda
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
| `OPENCODE_API_KEY`  | required     | OpenCode Zen key; genda exits 1 without it |
| `GENDA_ZEN_URL`     | `https://opencode.ai/zen/v1/systemone` | Zen System One endpoint |
| `GENDA_ZEN_MODEL`   | `jev-1.13-free` | Jev model id                          |

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

- Classifier is Jev via OpenCode Zen System One, one request per input:
  a `kind` choice (appointment / obligation / none, confidence =
  1 − P(none)) plus an `is_deadline` noul only when code found an ISO date.
  Dates are found in code, never by Jev; location is not extracted.
- When Jev fails (network, non-200, malformed reply) the raw input is
  still stored, `/ingest` answers 500 and the IMAP poller stops at that
  UID; the retry classifies again and never duplicates the event.
- IMAP poller marks nothing read; it tracks `imap_last_uid` in `meta`
  and skips already-seen UIDs.
- Future app: Kotlin Multiplatform against `GET /events`; website stays
  a simple page served separately.
