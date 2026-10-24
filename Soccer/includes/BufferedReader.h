//
// Created by Fabrizio Paino on 2026-05-15.
//
// BufferedReader<Stream>: HTTP-style buffered reads over a Soccer
// TcpStream / TlsStream (anything with a coroutine read(span<byte>) ->
// Task<size_t>; UdpSocket and RawSocket are datagram sockets with
// recvFrom / recv and are not streams, wrap them in a small adapter if
// needed). Provides line- and
// delimiter-bounded reads on top of fixed-byte read primitives, and a
// readExact that handles short reads automatically.
//
// The buffer is owned by the reader and refilled as needed. Callers
// see complete logical chunks (lines, framed records); the wire-level
// short-read juggling stays inside.
//

#ifndef SOCCER_BUFFERED_READER_H
#define SOCCER_BUFFERED_READER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Coroutines.h"
#include "SocketException.h"

namespace Soccer {

    namespace detail {
        /**
         * @brief Default buffer size for BufferedReader. 8 KiB matches
         *        the common HTTP / line-protocol payload size and keeps
         *        the buffer in L1 cache.
         */
        inline constexpr std::size_t kBufferedReaderDefaultCapacity = 8 * 1024;

        /**
         * @brief Hard cap on a single readLine / readUntilDelim
         *        result. Bounds memory use for adversarial inputs that
         *        never include the delimiter.
         */
        inline constexpr std::size_t kBufferedReaderMaxLine = 64 * 1024;

        /**
         * @brief Default ceiling on the @c n a single @c readExact will accept.
         *        The result is held in memory, so an unchecked length taken from
         *        the wire is a memory-exhaustion lever.
         */
        inline constexpr std::size_t kBufferedReaderMaxExact = 64 * 1024 * 1024;
    }


    /**
     * @class LineTooLongException
     * @brief Thrown by @ref BufferedReader::readUntilDelim / @c readLine when a
     *        record grows past the reader's @c maxLine without its delimiter.
     *        Distinct from other SocketExceptions so a protocol parser can map
     *        it to a "too large" reply instead of a generic I/O failure.
     */
    class LineTooLongException : public SocketException {
    public:
        explicit LineTooLongException(std::string message)
            : SocketException(std::move(message)) {
        }
    };


    /**
     * @class BufferedReader
     * @brief Buffered, coroutine-aware reader on top of any Soccer
     *        stream type with a @c read(std::span<std::byte>) ->
     *        @c YarnBall::Task<std::size_t> method.
     *
     * Uses a single contiguous byte buffer with read / write cursors.
     * Each public read drains pending buffered bytes first, then
     * refills via one underlying read per round-trip.
     *
     * Borrows (not owns) the underlying stream by pointer; the caller
     * is responsible for keeping it alive for the BufferedReader's
     * lifetime. Move-only.
     *
     * @tparam Stream Soccer stream type (TcpStream, TlsStream, etc.).
     */
    template<typename Stream>
    class BufferedReader final {
    public:
        /**
         * @param maxLine Longest line / delimited record @ref readUntilDelim
         *        will return; a longer one throws. Default 64 KiB.
         */
        explicit BufferedReader(Stream *stream,
                                std::size_t capacity =
                                detail::kBufferedReaderDefaultCapacity,
                                std::size_t maxLine = detail::kBufferedReaderMaxLine)
            : stream(stream), buf(capacity), readPos(0), writePos(0), maxLineBytes(maxLine) {
            // A zero-capacity buffer gives refill() zero headroom, so
            // it would return without ever calling stream->read() and
            // without setting eofObserved -- every subsequent
            // readExact/readUntilDelim would then see readPos ==
            // writePos still true and return an empty result forever,
            // indistinguishable from genuine EOF, even against a
            // healthy connection with data waiting. Reject it at
            // construction instead of degrading silently at first use.
            if (capacity == 0) {
                throw SocketException("BufferedReader: capacity must be > 0");
            }
        }

        BufferedReader(const BufferedReader &) = delete;
        BufferedReader &operator=(const BufferedReader &) = delete;

        BufferedReader(BufferedReader &&other) noexcept
            : stream(std::exchange(other.stream, nullptr)),
              buf(std::move(other.buf)),
              readPos(other.readPos),
              writePos(other.writePos),
              maxLineBytes(other.maxLineBytes),
              eofObserved(other.eofObserved) {
            other.readPos = 0;
            other.writePos = 0;
            other.eofObserved = false;
        }

        BufferedReader &operator=(BufferedReader &&other) noexcept {
            if (this != &other) {
                this->stream = std::exchange(other.stream, nullptr);
                this->buf = std::move(other.buf);
                this->readPos = other.readPos;
                this->writePos = other.writePos;
                this->maxLineBytes = other.maxLineBytes;
                this->eofObserved = other.eofObserved;
                other.readPos = 0;
                other.writePos = 0;
                other.eofObserved = false;
            }
            return *this;
        }

        ~BufferedReader() = default;

        /**
         * @return Underlying stream pointer (non-owning).
         */
        Stream *raw() const noexcept { return this->stream; }

        /**
         * @return Longest line / delimited record readUntilDelim will return.
         */
        std::size_t maxLine() const noexcept { return this->maxLineBytes; }

        /**
         * @brief Read exactly @p n bytes. Suspends as many times as
         *        necessary until the buffer is full. If the peer
         *        half-closes before @p n bytes arrive, returns the
         *        bytes it did receive (caller compares
         *        @c result.size() with @p n to detect a short read).
         *
         * @param maxBytes Largest @p n accepted (default 64 MiB). The result
         *        is buffered in memory, so a length taken from untrusted input
         *        must not be passed through unchecked.
         * @return Vector containing the bytes read.
         * @throws SocketException if @p n exceeds @p maxBytes (nothing is read).
         */
        YarnBall::Task<std::vector<std::byte>> readExact(
            std::size_t n, std::size_t maxBytes = detail::kBufferedReaderMaxExact) {
            if (n > maxBytes) {
                throw SocketException("BufferedReader::readExact: length exceeds bound");
            }
            std::vector<std::byte> out;
            // Do not trust n up front: a peer that announces a huge length and
            // then sends nothing must not pin n bytes per connection.
            out.reserve(std::min<std::size_t>(n, 64 * 1024));
            while (out.size() < n) {
                if (this->readPos == this->writePos) {
                    if (this->eofObserved) co_return out;
                    co_await this->refill();
                    if (this->readPos == this->writePos) co_return out;
                }
                const std::size_t want = n - out.size();
                const std::size_t avail = this->writePos - this->readPos;
                const std::size_t take = (want < avail) ? want : avail;
                out.insert(out.end(),
                           this->buf.data() + this->readPos,
                           this->buf.data() + this->readPos + take);
                this->readPos += take;
            }
            co_return out;
        }

        /**
         * @brief Read bytes up to and including the first @p delim. The
         *        returned string contains the data including the
         *        delimiter byte. Throws if the result would grow past the
         *        reader's @c maxLine() (constructor option, default
         *        @ref detail::kBufferedReaderMaxLine; prevents an
         *        adversarial peer from forcing unbounded memory use by never
         *        sending the delimiter).
         *
         * @param terminated Optional. Set to @c true when the result ends in
         *        @p delim and @c false when the stream hit EOF first: a
         *        non-empty result with @c *terminated == false is a partial
         *        record the peer never finished, which a protocol parser
         *        should usually reject rather than treat as complete.
         * @return The line as a @c std::string. Empty string on EOF
         *         before any byte arrived.
         */
        YarnBall::Task<std::string> readUntilDelim(std::byte delim, bool *terminated = nullptr) {
            if (terminated) *terminated = false;
            std::string out;
            while (true) {
                if (this->readPos == this->writePos) {
                    if (this->eofObserved) co_return out;
                    co_await this->refill();
                    if (this->readPos == this->writePos) co_return out;
                }
                // Scan the buffered region for @p delim.
                const std::byte *base = this->buf.data() + this->readPos;
                const std::size_t avail = this->writePos - this->readPos;
                std::size_t i = 0;
                while (i < avail && base[i] != delim) ++i;

                const std::size_t take = (i < avail) ? (i + 1) : i;
                if (out.size() + take > this->maxLineBytes) {
                    throw LineTooLongException(
                        "BufferedReader::readUntilDelim: line exceeds bound");
                }
                out.append(reinterpret_cast<const char *>(base), take);
                this->readPos += take;

                if (i < avail) { // delimiter consumed
                    if (terminated) *terminated = true;
                    co_return out;
                }
                // Else fall through and refill on next iteration.
            }
        }

        /**
         * @brief Convenience: read a line terminated by @c '\\n'. The
         *        terminator is included in the result (callers that
         *        want it stripped should pop it themselves). Empty on
         *        EOF before any byte arrived. See @ref readUntilDelim for
         *        @p terminated.
         */
        YarnBall::Task<std::string> readLine(bool *terminated = nullptr) {
            return this->readUntilDelim(std::byte{'\n'}, terminated);
        }

        /**
         * @brief @c true once the underlying stream has returned 0 from
         *        a read AND every previously buffered byte has been
         *        consumed.
         */
        bool eof() const noexcept {
            return this->eofObserved && this->readPos == this->writePos;
        }

    private:
        /**
         * @brief Refill the buffer with one underlying read. Compacts
         *        any unread bytes to the front first so the read sees
         *        @c capacity bytes of headroom.
         *
         * @note A single refill = a single underlying read = at most
         *       one Reactor await. Bigger refills don't help (the
         *       underlying stream returns whatever the kernel has)
         *       and would waste cache.
         */
        YarnBall::Task<void> refill() {
            if (this->readPos > 0) {
                if (this->readPos < this->writePos) {
                    const std::size_t live = this->writePos - this->readPos;
                    std::copy(this->buf.data() + this->readPos,
                              this->buf.data() + this->writePos,
                              this->buf.data());
                    this->writePos = live;
                } else {
                    this->writePos = 0;
                }
                this->readPos = 0;
            }
            const std::size_t headroom = this->buf.size() - this->writePos;
            if (headroom == 0) co_return; // pathological: buffer too small
            std::span<std::byte> tail(this->buf.data() + this->writePos,
                                       headroom);
            const std::size_t n = co_await this->stream->read(tail);
            if (n == 0) {
                this->eofObserved = true;
            } else {
                this->writePos += n;
            }
            co_return;
        }

        Stream *stream;
        std::vector<std::byte> buf;

        /// First unread byte in @c buf.
        std::size_t readPos;
        /// One past the last written byte in @c buf.
        std::size_t writePos;

        /// Longest record readUntilDelim returns.
        std::size_t maxLineBytes;

        bool eofObserved = false;
    };

}

#endif // SOCCER_BUFFERED_READER_H
