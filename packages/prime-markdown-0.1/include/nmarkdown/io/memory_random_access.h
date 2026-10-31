#ifndef NMARKDOWN_IO_MEMORY_RANDOM_ACCESS_H
#define NMARKDOWN_IO_MEMORY_RANDOM_ACCESS_H

#include <cstring>
#include <string>
#include <utility>

#include "nmarkdown/io/random_access.h"

namespace nmarkdown {

// Compatibility backing store for legacy-encoded TXT after GBK/JIS decoding.
// The TXT layout borrows this contiguous UTF-8 storage directly, so decoding
// does not cause a second whole-document allocation.
class MemoryRandomAccessData final : public RandomAccessData {
public:
    explicit MemoryRandomAccessData(std::string&& bytes)
        : bytes_(std::move(bytes)) {}

    std::uint64_t size() const override { return bytes_.size(); }
    const std::uint8_t* contiguous_data() const override {
        return reinterpret_cast<const std::uint8_t*>(bytes_.data());
    }

    bool read(std::uint64_t offset,
              std::uint8_t* data,
              std::size_t size) override {
        if (offset > bytes_.size() || size > bytes_.size() - offset ||
            (data == nullptr && size != 0)) {
            return false;
        }
        if (size != 0) {
            std::memcpy(data, bytes_.data() + static_cast<std::size_t>(offset),
                        size);
        }
        return true;
    }

private:
    std::string bytes_;
};

}  // namespace nmarkdown

#endif
