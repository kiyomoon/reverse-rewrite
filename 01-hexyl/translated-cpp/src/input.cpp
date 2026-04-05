#include "input.hpp"

#include <cerrno>
#include <stdexcept>

namespace hexyl {

Input Input::from_file(const std::string& path) {
    Input inp;
    inp.file_ = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!inp.file_->is_open()) {
        throw std::runtime_error("Could not open file: " + path);
    }
    inp.is_file_ = true;
    return inp;
}

Input Input::from_stdin() {
    Input inp;
    inp.is_file_ = false;
    // stdin is not owned; file_ stays null
    return inp;
}

std::istream& Input::stream() {
    if (is_file_) {
        return *file_;
    }
    return std::cin;
}

// Try to skip `count` bytes by reading and discarding
static uint64_t try_skip(std::istream& s, uint64_t count) {
    uint64_t total = 0;
    char buf[4096];
    while (total < count) {
        auto to_read = static_cast<std::streamsize>(
            std::min<uint64_t>(count - total, sizeof(buf)));
        s.read(buf, to_read);
        auto n = s.gcount();
        if (n == 0) break;
        total += static_cast<uint64_t>(n);
    }
    return total;
}

uint64_t Input::seek(int kind, int64_t offset) {
    auto& s = stream();

    if (is_file_) {
        // Try real seek first
        std::ios::seekdir dir;
        switch (kind) {
            case 0: dir = std::ios::cur; break;
            case 1: dir = std::ios::beg; break;
            case 2: dir = std::ios::end; break;
            default: throw std::runtime_error("invalid seek kind");
        }
        s.clear(); // clear any error flags
        s.seekg(offset, dir);
        if (!s.fail()) {
            return static_cast<uint64_t>(s.tellg());
        }
        // seekg failed — probably a pipe. Fall back to skip-forward.
        s.clear();
        if (kind == 0 && offset >= 0) {
            return try_skip(s, static_cast<uint64_t>(offset));
        }
        throw std::runtime_error(
            "Pipes only support seeking forward with a relative offset");
    }

    // Stdin: only forward-relative seeks
    if (kind != 0 || offset < 0) {
        throw std::runtime_error(
            "STDIN only supports seeking forward with a relative offset");
    }
    return try_skip(s, static_cast<uint64_t>(offset));
}

std::unique_ptr<std::istream> Input::into_inner() {
    if (is_file_) {
        return std::move(file_);
    }
    // For stdin, create a thin wrapper that shares cin's streambuf.
    // This is safe because cin outlives any usage.
    class StdinWrapper : public std::istream {
    public:
        StdinWrapper() : std::istream(std::cin.rdbuf()) {}
    };
    return std::make_unique<StdinWrapper>();
}

// --- LimitedReader ---

LimitedReader::LimitedBuf::LimitedBuf(std::istream& inner, uint64_t limit)
    : inner_(inner), remaining_(limit) {
    // Start with empty buffer
    setg(buf_, buf_, buf_);
}

LimitedReader::LimitedBuf::int_type LimitedReader::LimitedBuf::underflow() {
    if (remaining_ == 0) {
        return traits_type::eof();
    }
    auto to_read = static_cast<std::streamsize>(
        std::min<uint64_t>(remaining_, sizeof(buf_)));
    inner_.read(buf_, to_read);
    auto n = inner_.gcount();
    if (n == 0) {
        return traits_type::eof();
    }
    remaining_ -= static_cast<uint64_t>(n);
    setg(buf_, buf_, buf_ + n);
    return traits_type::to_int_type(buf_[0]);
}

LimitedReader::LimitedReader(std::unique_ptr<std::istream> inner, uint64_t limit)
    : std::istream(&buf_), inner_(std::move(inner)), buf_(*inner_, limit) {}

} // namespace hexyl
