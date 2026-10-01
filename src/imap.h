// Best-effort IMAP poller feeding the same ingest path as /ingest.
// No-op unless GENDA_IMAP_URL is set. Never marks mail read.
#ifndef GENDA_IMAP_H
#define GENDA_IMAP_H

void *imapThread(void *arg);

#endif
