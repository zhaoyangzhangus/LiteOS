# P5 design notes

## Why not enable parallel drag

A drag has visible move-transaction semantics and still lacks a real page-flip
contract. Keep one scanout writer for that path.

## Why 1M pixels

The existing generic parallel helper accepts >512K pixels. P5 deliberately
uses a stricter 1M-pixel threshold for ordinary repaint so worker wake and
completion synchronization are amortized over at least ~4 MiB of XRGB data.

This value is a tuning point, not an ABI.

## Why no extra SFENCE after worker completion

Participant zero fences its own WC stores. Each helper fences before its
release-completion increment. The compositor waits with acquire loads. The
existing helper contract therefore already guarantees publication completion
without another local fence ordering remote CPU stores.
