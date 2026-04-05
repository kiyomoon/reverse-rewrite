#pragma once

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace hexyl {

// Input abstraction wrapping either a file or stdin.
// Translates Rust's `enum Input<'a> { File(fs::File), Stdin(io::StdinLock<'a>) }`
// into a C++ class using std::istream polymorphism.
//
// The key behavior to preserve: seeking on non-seekable streams (pipes, stdin)
// falls back to reading-and-discarding for forward seeks.
class Input {
public:
    // Open a file for reading
    static Input from_file(const std::string& path);

    // Use stdin as input
    static Input from_stdin();

    // Get the underlying istream
    std::istream& stream();

    // Seek to a position. For non-seekable streams, forward seeks are emulated
    // by reading and discarding bytes. Returns the new position (or bytes skipped
    // for non-seekable streams).
    //
    // kind: 0 = from current (SeekFrom::Current), 1 = from start (SeekFrom::Start),
    //       2 = from end (SeekFrom::End)
    uint64_t seek(int kind, int64_t offset);

    // Release the istream as a unique_ptr (for use with take/limit wrappers)
    // Corresponds to Rust's Input::into_inner() -> Box<dyn Read>
    std::unique_ptr<std::istream> into_inner();

    bool is_file() const { return is_file_; }

private:
    Input() = default;

    std::unique_ptr<std::ifstream> file_;
    bool is_file_ = false;
};

// A wrapper that limits reading to at most `limit` bytes.
// Corresponds to Rust's reader.take(n).
class LimitedReader : public std::istream {
public:
    LimitedReader(std::unique_ptr<std::istream> inner, uint64_t limit);

    // Not copyable
    LimitedReader(const LimitedReader&) = delete;
    LimitedReader& operator=(const LimitedReader&) = delete;

private:
    class LimitedBuf : public std::streambuf {
    public:
        LimitedBuf(std::istream& inner, uint64_t limit);

    protected:
        int_type underflow() override;

    private:
        std::istream& inner_;
        uint64_t remaining_;
        char buf_[4096];
    };

    std::unique_ptr<std::istream> inner_;
    LimitedBuf buf_;
};

} // namespace hexyl
