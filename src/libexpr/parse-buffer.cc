#include "nix/expr/parse-buffer.hh"
#include "nix/util/error.hh"
#include "nix/util/serialise.hh"

#include "expr-config-private.hh"

#include <cerrno>
#include <cstring>
#include <new>
#include <optional>

#include <fcntl.h>
#include <sys/mman.h>

#if HAVE_LIBURING
#  include <liburing.h>
#endif

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

/* Through the accessor, for files that have no physical path or when
   io_uring is not there */
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

#if HAVE_LIBURING

/* A ring per thread, with a table of direct descriptors so that a file's
   open, read and close go in as one linked chain and no descriptor ever
   reaches the process's table */
struct Ring
{
    static constexpr unsigned slots = 32;

    io_uring ring;
    bool ok = false;

    Ring()
    {
        io_uring_params params{};
        params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
        if (io_uring_queue_init_params(slots * 4, &ring, &params) < 0) {
            /* Kernels before 6.1 have neither flag */
            params = {};
            if (io_uring_queue_init_params(slots * 4, &ring, &params) < 0)
                return;
        }
        if (io_uring_register_files_sparse(&ring, slots) < 0) {
            io_uring_queue_exit(&ring);
            return;
        }
        ok = true;
    }

    ~Ring()
    {
        if (ok)
            io_uring_queue_exit(&ring);
    }

    Ring(const Ring &) = delete;
    Ring & operator=(const Ring &) = delete;
};

thread_local Ring ring;

enum Op : uint64_t { OpOpen, OpRead, OpClose };

/* Read `paths` into `out` with one submission. A file that this cannot
   read, or whose read comes back short, is left empty, for the caller to
   read through its accessor */
void readBatch(
    Ring & r,
    std::span<const SourcePath> paths,
    std::span<std::optional<ParseBuffer>> out)
{
    struct Pending
    {
        size_t i;
        std::string phys;
        int openRes = 0;
        int readRes = -1;
    };

    std::vector<Pending> pending;
    pending.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); i++) {
        auto & path = paths[i];
        try {
            /* The accessor's own checks: whether the path is allowed at
               all, that it is a regular file, and its mtime where it keeps
               one */
            auto st = path.lstat();
            if (st.type != SourceAccessor::tRegular || !st.fileSize || *st.fileSize > (size_t(1) << 30))
                continue;
            auto phys = path.getPhysicalPath();
            if (!phys)
                continue;
            auto physStr = phys->string();
            out[i] = ParseBuffer(size_t(*st.fileSize));
            pending.push_back(Pending{.i = i, .phys = std::move(physStr)});
        } catch (...) {
            /* Left to the accessor's `readFile`, so the error is the one
               it always gave */
        }
    }
    if (pending.empty())
        return;

    for (unsigned slot = 0; slot < pending.size(); slot++) {
        auto & pe = pending[slot];
        auto & buf = *out[pe.i];
        uint64_t tag = uint64_t(slot) << 2;

        auto sqe = io_uring_get_sqe(&r.ring);
        io_uring_prep_openat_direct(sqe, AT_FDCWD, pe.phys.c_str(), O_RDONLY | O_NOFOLLOW, 0, slot);
        sqe->flags |= IOSQE_IO_LINK;
        io_uring_sqe_set_data64(sqe, tag | OpOpen);

        sqe = io_uring_get_sqe(&r.ring);
        io_uring_prep_read(sqe, slot, buf.data(), buf.size(), 0);
        /* A hard link, so that the close runs after a failed read too */
        sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
        io_uring_sqe_set_data64(sqe, tag | OpRead);

        sqe = io_uring_get_sqe(&r.ring);
        io_uring_prep_close_direct(sqe, slot);
        io_uring_sqe_set_data64(sqe, tag | OpClose);
    }

    unsigned want = pending.size() * 3;
    int submitted;
    do
        submitted = io_uring_submit_and_wait(&r.ring, want);
    while (submitted == -EINTR);
    if (submitted < 0) {
        /* The kernel took none of them, but they are still queued, so the
           ring cannot be used again */
        r.ok = false;
        for (auto & pe : pending)
            out[pe.i].reset();
        return;
    }
    if (unsigned(submitted) < want)
        r.ok = false;

    /* Every request the kernel took writes a completion, a cancelled one
       too, and the buffers must outlive them all */
    for (int seen = 0; seen < submitted;) {
        io_uring_cqe * cqe;
        int err = io_uring_wait_cqe(&r.ring, &cqe);
        if (err == -EINTR)
            continue;
        if (err < 0) {
            /* Reads may still land in the buffers, so leak them */
            r.ok = false;
            auto leaked = new std::vector<ParseBuffer>;
            for (auto & pe : pending) {
                leaked->push_back(std::move(*out[pe.i]));
                out[pe.i].reset();
            }
            return;
        }
        auto tag = io_uring_cqe_get_data64(cqe);
        auto & pe = pending[tag >> 2];
        switch (tag & 3) {
        case OpOpen:
            pe.openRes = cqe->res;
            break;
        case OpRead:
            pe.readRes = cqe->res;
            break;
        default:
            break;
        }
        io_uring_cqe_seen(&r.ring, cqe);
        seen++;
    }

    for (auto & pe : pending)
        if (pe.openRes < 0 || pe.readRes < 0 || size_t(pe.readRes) != out[pe.i]->size())
            out[pe.i].reset();
}

#endif

} // namespace

std::vector<std::variant<ParseBuffer, std::exception_ptr>> readParseBuffers(std::span<const SourcePath> paths)
{
    std::vector<std::optional<ParseBuffer>> got(paths.size());

#if HAVE_LIBURING
    for (size_t at = 0; at < paths.size() && ring.ok; at += Ring::slots) {
        size_t n = std::min<size_t>(Ring::slots, paths.size() - at);
        readBatch(ring, paths.subspan(at, n), std::span(got).subspan(at, n));
    }
#endif

    std::vector<std::variant<ParseBuffer, std::exception_ptr>> res;
    res.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); i++) {
        if (got[i]) {
            res.push_back(std::move(*got[i]));
            continue;
        }
        try {
            res.emplace_back(readThroughAccessor(paths[i]));
        } catch (...) {
            res.emplace_back(std::current_exception());
        }
    }
    return res;
}

ParseBuffer readParseBuffer(const SourcePath & path)
{
    auto res = readParseBuffers(std::span(&path, 1));
    if (auto e = std::get_if<std::exception_ptr>(&res[0]))
        std::rethrow_exception(*e);
    return std::move(std::get<ParseBuffer>(res[0]));
}

} // namespace nix
