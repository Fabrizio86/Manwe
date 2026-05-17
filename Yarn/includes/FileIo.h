//
// Created by Fabrizio Paino on 2026-05-16.
//
// YarnBall::fs -- coroutine-friendly file I/O. The implementation hops
// to a Yarn worker via @c scheduleOn, performs the blocking std::fstream
// call, and resumes the caller. Portable, correct, and good for the
// common case (config files, log lines, small reads/writes interleaved
// with network work).
//
// Why worker-hop instead of native async:
//
//   - macOS / *BSD: kqueue does NOT support readiness notifications
//     for regular files. Files are always "ready"; reads block in the
//     kernel. There is no portable POSIX async file API on this
//     platform, so worker-hop is the only option.
//   - Linux epoll: same -- regular files don't generate epoll events.
//     epoll's POLLIN never fires for files.
//   - Linux io_uring: DOES support async file I/O via IORING_OP_READ
//     / IORING_OP_WRITE / IORING_OP_OPENAT2. A native io_uring file
//     path would skip the worker hop and is a clean perf win for
//     high-fanout file workloads; it's a planned follow-up round
//     and not in scope here.
//   - Windows IOCP: supports OVERLAPPED ReadFile / WriteFile on file
//     handles. Same story as Linux io_uring: planned follow-up.
//
// At very large scale a dedicated "blocking I/O" pool sized for the
// expected file-IO concurrency would be a better fit than sharing
// Yarn's compute workers; that's also future work.
//

#ifndef YARN_FILE_IO_H
#define YARN_FILE_IO_H

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Coroutines.h"
#include "Yarn.hpp"

namespace YarnBall::fs {

    /**
     * @class FileIoError
     * @brief Thrown by @ref File operations on syscall failures.
     */
    class FileIoError : public std::runtime_error {
    public:
        explicit FileIoError(const std::string &what)
            : std::runtime_error("FileIoError: " + what) {
        }
    };

    /**
     * @enum OpenMode
     * @brief Equivalent of @c std::ios_base modes, simplified to the
     *        common cases coroutine code actually uses.
     */
    enum class OpenMode {
        Read,       ///< Open existing file for reading.
        Write,      ///< Truncate / create for writing.
        Append,     ///< Create if missing; writes append at end.
        ReadWrite,  ///< Open / create, no truncate, read + write at any offset.
    };

    /**
     * @class File
     * @brief Move-only file handle. Reads / writes hop to a Yarn
     *        worker, perform a blocking @c std::fstream operation,
     *        then resume the caller.
     *
     * Use the free convenience helpers (@ref readToString /
     * @ref writeBytes) for one-shot reads / writes; the @c File
     * type itself is for streaming patterns where the caller wants
     * incremental I/O.
     */
    class File final {
    public:
        File() noexcept = default;

        File(const File &) = delete;
        File &operator=(const File &) = delete;

        File(File &&other) noexcept = default;
        File &operator=(File &&other) noexcept = default;

        ~File() = default;

        /**
         * @brief Open @p path with the requested @p mode. Suspends
         *        on a Yarn worker for the duration of the underlying
         *        @c std::fstream::open call.
         */
        static YarnBall::Task<File> open(std::string path, OpenMode mode) {
            co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
            File f;
            std::ios_base::openmode flags = std::ios_base::binary;
            switch (mode) {
                case OpenMode::Read:
                    flags |= std::ios_base::in;
                    break;
                case OpenMode::Write:
                    flags |= std::ios_base::out | std::ios_base::trunc;
                    break;
                case OpenMode::Append:
                    flags |= std::ios_base::out | std::ios_base::app;
                    break;
                case OpenMode::ReadWrite:
                    flags |= std::ios_base::in | std::ios_base::out;
                    break;
            }
            f.stream.open(path, flags);
            if (!f.stream.is_open()) {
                throw FileIoError("open(" + path + ") failed");
            }
            co_return f;
        }

        bool isOpen() const noexcept { return this->stream.is_open(); }

        /**
         * @brief Read up to @p buf.size() bytes. Returns 0 on EOF.
         *        Throws @ref FileIoError on a non-EOF error.
         */
        YarnBall::Task<std::size_t> read(std::span<std::byte> buf) {
            co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
            this->stream.read(reinterpret_cast<char *>(buf.data()),
                              static_cast<std::streamsize>(buf.size()));
            const auto n = this->stream.gcount();
            if (n == 0 && !this->stream.eof() && this->stream.fail()) {
                throw FileIoError("read failed");
            }
            // Clear EOF/fail so subsequent operations (e.g. seekg)
            // start from a clean stream state. We surface EOF via
            // the return value being 0, not via the stream flags.
            this->stream.clear();
            co_return static_cast<std::size_t>(n);
        }

        /**
         * @brief Write @p buf in full. Throws on partial write or
         *        on syscall failure.
         */
        YarnBall::Task<std::size_t> write(std::span<const std::byte> buf) {
            co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
            this->stream.write(reinterpret_cast<const char *>(buf.data()),
                               static_cast<std::streamsize>(buf.size()));
            if (this->stream.fail()) {
                throw FileIoError("write failed");
            }
            co_return buf.size();
        }

        /**
         * @brief Flush buffered writes to the OS (does NOT call fsync).
         */
        YarnBall::Task<void> flush() {
            co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
            this->stream.flush();
            if (this->stream.fail()) {
                throw FileIoError("flush failed");
            }
            co_return;
        }

        /**
         * @brief Close the file. Idempotent.
         */
        YarnBall::Task<void> close() {
            if (!this->stream.is_open()) co_return;
            co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
            this->stream.close();
            co_return;
        }

    private:
        std::fstream stream;
    };


    // ---- Convenience one-shot helpers --------------------------------

    /**
     * @brief Read @p path in full, returning the bytes as a string.
     *        Useful for small files (config, etc.). Throws on
     *        open / read failure.
     */
    inline YarnBall::Task<std::string> readToString(std::string path) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {
            throw FileIoError("readToString(" + path + "): open failed");
        }
        std::string out((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        if (f.bad()) {
            throw FileIoError("readToString(" + path + "): read failed");
        }
        co_return out;
    }

    /**
     * @brief Read @p path in full, returning the bytes as a vector.
     */
    inline YarnBall::Task<std::vector<std::byte>> readToBytes(std::string path) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {
            throw FileIoError("readToBytes(" + path + "): open failed");
        }
        std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (f.bad()) {
            throw FileIoError("readToBytes(" + path + "): read failed");
        }
        std::vector<std::byte> out(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            out[i] = static_cast<std::byte>(raw[i]);
        }
        co_return out;
    }

    /**
     * @brief Write @p data to @p path. Truncates existing content.
     */
    inline YarnBall::Task<void> writeString(std::string path,
                                              std::string data) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            throw FileIoError("writeString(" + path + "): open failed");
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (f.fail()) {
            throw FileIoError("writeString(" + path + "): write failed");
        }
        co_return;
    }

    /**
     * @brief Write @p data to @p path. Truncates existing content.
     */
    inline YarnBall::Task<void> writeBytes(std::string path,
                                             std::span<const std::byte> data) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            throw FileIoError("writeBytes(" + path + "): open failed");
        }
        f.write(reinterpret_cast<const char *>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (f.fail()) {
            throw FileIoError("writeBytes(" + path + "): write failed");
        }
        co_return;
    }

    /**
     * @brief Delete @p path. Throws if the path does not exist.
     */
    inline YarnBall::Task<void> remove(std::string path) {
        co_await YarnBall::scheduleOn(YarnBall::Yarn::instance());
        if (std::remove(path.c_str()) != 0) {
            throw FileIoError("remove(" + path + ") failed");
        }
        co_return;
    }

}

#endif // YARN_FILE_IO_H
