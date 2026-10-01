// Plain-HTTP server on loopback: /health, POST /ingest, GET /events.
// TLS terminates outside (Caddy/Tailscale).
#ifndef GENDA_SERVER_H
#define GENDA_SERVER_H

// Bind, listen, serve until a fatal socket error. Returns 1 on fatal error.
int server_run(void);

#endif
