# Wishlist from building database clients on Manwe

Findings from writing a MySQL client and a SQL Server (TDS) client, and from planning an SSH client, on top of Manwe in a closed
desktop and tablet application (Lindale, `~/CLionProjects/Lindale`, branch `database-panel`). Written 2026-10-15. Nothing here was
changed in Manwe; this is a list of things to decide. Items are ordered by how much work they would have saved.

The goal stated for this list: Manwe should be a library to build network software from, so that a developer writing a protocol
client does not have to write the plumbing again.

## 1. Crypto is missing entirely

Manwe has no hash, MAC, cipher, key exchange or signature code. Any serious protocol client needs some. What Lindale had to write
itself (all plain C++, no dependency, tested against Python `hashlib` vectors; see `core/db/Sha1.*`, `Sha256.*`, `MySqlScramble.*`):

| Needed | Used for | Where Lindale got it |
|---|---|---|
| SHA-1, SHA-256 | MySQL password scrambles, fingerprints of certificates | wrote `Sha1`, `Sha256` (about 100 lines each) |
| HMAC, HKDF, PBKDF2 | SSH key derivation, SCRAM logins (PostgreSQL, MongoDB), token signing | not yet written |
| SHA-512, SHA-384 | Ed25519, TLS-adjacent signatures, SSH host key hashes | not yet written |
| AES (CTR, GCM), ChaCha20-Poly1305 | SSH transport, encrypted storage | system C APIs on Apple only (CommonCrypto has no ChaCha) |
| X25519, Ed25519 | SSH key exchange and the most common SSH user keys | no system C API on Apple (CryptoKit is Swift only); needs own code |
| ECDH/ECDSA P-256, RSA (OAEP, PKCS1, PSS) | MySQL full authentication without TLS, SSH host keys | Apple Security framework through a small wrapper |
| CSPRNG | nonces, keys, session ids | `arc4random_buf` / `getrandom` / `BCryptGenRandom` wrapper |
| Constant-time compare and wipe | comparing MACs, clearing secrets | trivial, but it should exist once and be right |

Suggestion: a `Crypto` module with a portable C++ core for the hashes, MACs, KDFs, ChaCha20-Poly1305, X25519 and Ed25519, and a
`Crypto::System` layer that uses the platform (CommonCrypto/Security on Apple, CNG on Windows, the kernel or OpenSSL-free primitives
on Linux) for AES, RSA and ECDSA when it is faster or hardware backed. Test vectors from the RFCs and Wycheproof in `YarnTests`.
Everything with a span-in, array-out interface so it composes with `BufferedReader`.

## 2. TLS: one backend, connect-time only

`TlsStream` is built on libtls (LibreSSL). Findings:

* **It can only start TLS at connect.** MySQL (SSL request after the plain greeting), SQL Server 2017 and older (TLS records wrapped
  inside PRELOGIN packets), PostgreSQL (`SSLRequest`), SMTP/IMAP/FTP `STARTTLS` and LDAP all begin in clear text and upgrade the same
  socket. Wanted: `TlsStream::upgrade(TcpStream&&, TlsClientOptions)`.
* **No record framing hook.** The SQL Server 7.4 handshake travels inside 8-byte-header packets of type 0x12. A pluggable
  "transport under TLS" (two callbacks, read some / write some) would let a protocol wrap the handshake without forking TLS code.
* **No verification callback and no certificate access.** Needed: the peer's leaf certificate (DER) and its SHA-256 fingerprint, a
  callback that can accept or refuse after the chain check (trust-this-server-once with pinning, as SSMS and DBeaver do), host name
  override (SQL Server `HostNameInCertificate`), and a way to say "encrypt but do not verify" without the word insecure in every call.
* **libtls is a second TLS stack.** Apple platforms have Security.framework (SecureTransport, still present; Network.framework cannot
  upgrade a live socket). A backend interface with `Libtls`, `Apple` (SecureTransport) and `SChannel` implementations would remove the
  LibreSSL dependency from iOS and macOS builds, where it also cannot use the system trust store policy, and would let apps choose.
  Lindale wrote this as `platform/apple/shared/SocketTransport.mm` (about 450 lines, SecureTransport, cancellation-safe callbacks,
  TDS framing, fingerprint pinning, ALPN `tds/8.0`); it is a reasonable starting point.
* **Cancellation of a handshake.** A cancel while a TLS record is half read corrupts the context unless the read callback returns
  "would block" before the first byte only. Lindale's rule (see item 4) is worth building into the backend.

## 3. A stream abstraction that is not "TcpStream or TlsStream"

Protocol code wants one type for "something I can read and write bytes on". Today `BufferedReader<Stream>` is generic by template,
but nothing else is. Wanted: a `concept ByteStream` (read, write, close, and optionally `setKeepAlive`, `peerFingerprint`,
`isSecure`) plus a type-erased `AnyStream` for the places where templates are a burden (a client class that can run on TCP, TLS,
a Unix socket, an SSH forwarded channel, or an in-memory test stream). With it, an SSH direct-tcpip channel is just another
`AnyStream` and the database clients need no change.

## 4. Cancel-safe exact reads

A protocol reader must be able to give up waiting for the next packet without losing a half-read packet. What worked:

* a `readExact(n)` whose cancel and timeout are honoured only before its first byte, and which then finishes or fails with
  "peer stalled" (30 s default);
* readers that keep their own progress, so a cancelled `readPacket()` can be called again later and carries on;
* units of at most 16 KB, so a big value does not delay a cancel for long.

`BufferedReader::readExact` exists; it should document and guarantee this behaviour, and offer the "resume after cancel" form.

## 5. Socket options and plumbing

* `TcpStream::setKeepAlive(idle, interval, count)`, `setNoDelay`, `setNoSigpipe` (Apple), receive and send buffer sizes. Lindale
  sets them with raw `setsockopt` after the fact. Keepalive at the TCP level matters: a database client must not poll with
  queries (it fills server logs and keeps serverless databases awake).
* `TcpStream::bytesAvailable()` (non-blocking "would a read wait?"). A streaming result reader uses it to hand rows to the UI as
  soon as the network goes quiet, instead of waiting for a full batch.
* Asynchronous name resolution (`SocketAddress::resolve` is synchronous). Mentioned in the header already; an off-thread
  `co_await resolve(host)` that honours cancellation would remove the one uninterruptible step.
* Happy Eyeballs for hosts with both IPv4 and IPv6.

## 6. Byte codecs for protocol work

Every binary protocol client needs the same small tools, and each one was rewritten:

* a bounds-checked, sticky-failure `ByteReader` / `ByteWriter` with little- and big-endian integers, length-encoded integers
  (MySQL style), varints, NUL-terminated and length-prefixed strings, "take n bytes or fail" and "skip n";
* UTF-8 / UTF-16 / UTF-32 / Windows-1252 conversion (SQL Server speaks UTF-16LE; Lindale wrote `Utf16`);
* hex, Base64 (with and without padding and line breaks), PEM and a minimal DER walker;
* a packet framing helper (header with length and sequence, split and join at a maximum size);
* date helpers: days to civil date and back without `time_t`.

The reader design (never reads past the end, one sticky error, typed) is what makes hostile-server testing possible; it deserves to be
the one blessed way to parse untrusted bytes. See `core/db/MySqlByteReader.*`, `MySqlByteWriter.*`, `TdsPacketStream.*`.

## 7. Test support for network code

What made the database clients testable without a network, and what Manwe could ship in a `Soccer::Testing` header:

* an in-memory duplex stream pair (`MemoryStream::pair()`) and a scripted stream whose "server" is a function of what the client
  wrote (Lindale: `FakeMySqlServer`, `FakeTdsServer`);
* fault injection on any stream: cut after n bytes, stall, flip a bit, split writes into one-byte pieces, delay;
* a deterministic clock for timers and deadlines;
* a libFuzzer entry-point helper and a build preset for it (Apple's clang ships no libFuzzer; upstream LLVM does).

## 8. Client building blocks above the socket

* async connection pool with health checks, maximum age, idle timeout and a fair waiter queue (`Pool<T>`);
* retry with backoff and jitter, circuit breaker, per-call deadline propagated through cancellation scopes;
* an idle disconnect / reconnect helper ("close after N minutes of silence, reconnect quietly on the next use");
* a streaming-result pattern: producer coroutine, bounded channel, consumer that may stop early, memory cap that stops the fetch
  without an error (Lindale's `IResultSink::rows()` returning false). `Wire::BoundedChannel` is close; a documented recipe would do.

## 9. Protocol clients themselves

Once items 1 to 8 exist, these could live in Manwe as optional modules, because every service developer needs one of them:

* MySQL / MariaDB client (done in Lindale: handshake, native and caching_sha2 login, TLS upgrade, text and binary protocol,
  prepared statements, KILL QUERY cancel). PostgreSQL next (SCRAM-SHA-256 needs item 1).
* TDS (SQL Server, Azure SQL) client (done in Lindale: PRELOGIN, TLS in PRELOGIN and TDS 8.0 strict, LOGIN7, Entra token,
  routing, every type, ATTENTION cancel, sp_executesql).
* SSH client (transport, user auth with ed25519, ecdsa and rsa keys, `direct-tcpip` forwarding, host key checking with a callback).
  Lindale plans to write this on top of item 1.
* Redis (RESP3), MongoDB wire protocol, SMTP/IMAP, MQTT: small once item 6 exists.

Lindale's versions are not Manwe-shaped yet (blocking, run on a pool worker, with a `Git::CancelToken`). Moving them onto
`Task<>`, `TcpStream` and `withCancellation` is the plan; they would then be directly usable here.

## 10. Smaller notes

* `TlsStream.h` is compiled out without `SOCCER_HAS_TLS`; a build that can tell the app at runtime which backends exist would be easier.
* `HttpClient` has no way to pin a certificate or supply a trust callback (same as item 2).
* `Metrics` and `Trace` would be useful per connection (bytes, round trips, time to first row); a small "connection stats" struct
  that every stream fills in would let applications show them.
* Documentation: a "write a protocol client" walkthrough using one of the clients above as the example.
