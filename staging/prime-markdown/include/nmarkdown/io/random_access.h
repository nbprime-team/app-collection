#ifndef NMARKDOWN_IO_RANDOM_ACCESS_H
#define NMARKDOWN_IO_RANDOM_ACCESS_H

#include <cstddef>
#include <cstdint>

namespace nmarkdown {

// Ownership-safe random access to a large byte resource. External fonts use
// this to avoid retaining their complete payload in calculator RAM.
class RandomAccessData {
public:
    virtual ~RandomAccessData() = default;
    virtual std::uint64_t size() const = 0;
    // Memory-backed adapters may expose a stable contiguous view so callers
    // can avoid creating a second whole-file cache.
    virtual const std::uint8_t* contiguous_data() const { return nullptr; }
    virtual bool read(std::uint64_t offset,
                      std::uint8_t* data,
                      std::size_t size) = 0;
};

}  // namespace nmarkdown

#endif
