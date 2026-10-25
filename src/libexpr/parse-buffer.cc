#include "nix/expr/parse-buffer.hh"
#include "nix/util/error.hh"
#include "nix/util/serialise.hh"

#include <cstring>
#include <new>
#include <optional>

#include <sys/mman.h>

namespace nix {

namespace {

constexpr size_t hugePage = size_t(2) << 20;
constexpr std::align_val_t blockAlign{64};

} // namespace

ParseBuffer::ParseBuffer(size_t len)
    : len(len)
{
    cap = ((len + 63) & ~size_t(63)) + padding;
    if (cap >= hugePage) {
        cap = (cap + hugePage - 1) & ~(hugePage - 1);
        void * m = mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
            throw std::bad_alloc();
#ifdef MADV_HUGEPAGE
        madvise(m, cap, MADV_HUGEPAGE);
#endif
        /* Fresh anonymous pages read as zero, padding included */
        p = static_cast<char *>(m);
    } else {
        p = static_cast<char *>(::operator new(cap, blockAlign));
        memset(p + len, 0, cap - len);
    }
}

ParseBuffer::ParseBuffer(std::string_view s)
    : ParseBuffer(s.size())
{
    memcpy(p, s.data(), s.size());
}

ParseBuffer::ParseBuffer(ParseBuffer && other) noexcept
    : p(std::exchange(other.p, nullptr))
    , len(std::exchange(other.len, 0))
    , cap(std::exchange(other.cap, 0))
{
}

ParseBuffer & ParseBuffer::operator=(ParseBuffer && other) noexcept
{
    if (this != &other) {
        this->~ParseBuffer();
        new (this) ParseBuffer(std::move(other));
    }
    return *this;
}

ParseBuffer::~ParseBuffer()
{
    if (!p)
        return;
    if (cap >= hugePage)
        munmap(p, cap);
    else
        ::operator delete(p, blockAlign);
}

namespace {

ParseBuffer readThroughAccessor(const SourcePath & path)
{
    struct BufferSink : Sink
    {
        const SourcePath & path;
        std::optional<ParseBuffer> buf;
        size_t at = 0;

        BufferSink(const SourcePath & path)
            : path(path)
        {
        }

        void operator()(std::string_view data) override
        {
            if (!buf || at + data.size() > buf->size())
                throw Error("file '%s' changed size while it was read", path);
            memcpy(buf->data() + at, data.data(), data.size());
            at += data.size();
        }
    };

    BufferSink sink(path);
    path.readFile(sink, [&](uint64_t size) { sink.buf.emplace(size); });
    if (!sink.buf)
        return ParseBuffer(path.readFile());
    if (sink.at != sink.buf->size())
        throw Error("file '%s' changed size while it was read", path);
    return std::move(*sink.buf);
}

} // namespace

ParseBuffer readParseBuffer(const SourcePath & path)
{
    return readThroughAccessor(path);
}

} // namespace nix
