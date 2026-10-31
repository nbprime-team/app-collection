#ifndef NMARKDOWN_IO_BLOCK_CACHED_RANDOM_ACCESS_H
#define NMARKDOWN_IO_BLOCK_CACHED_RANDOM_ACCESS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "nmarkdown/io/random_access.h"

namespace nmarkdown {

// Small LRU block cache for storage-backed random-access resources. External
// OpenType fonts make many tiny, repeated seeks through cmap/loca/hmtx/glyf;
// satisfying those from RAM prevents every cold glyph from becoming a burst
// of calculator filesystem reads.
class BlockCachedRandomAccessData final : public RandomAccessData {
public:
    BlockCachedRandomAccessData(
        std::shared_ptr<RandomAccessData> source,
        std::size_t cache_bytes = 32U * 1024U,
        std::size_t block_bytes = 1024U)
        : source_(std::move(source)),
          block_bytes_(std::max<std::size_t>(1, block_bytes)) {
        const std::size_t block_count = std::max<std::size_t>(
            1, cache_bytes / block_bytes_);
        bytes_.resize(block_count * block_bytes_);
        slots_.resize(block_count);
    }

    std::uint64_t size() const override {
        return source_ == nullptr ? 0 : source_->size();
    }

    bool read(std::uint64_t offset,
              std::uint8_t* data,
              std::size_t size) override {
        if (source_ == nullptr || (data == nullptr && size != 0) ||
            offset > source_->size() ||
            size > source_->size() - offset) {
            return false;
        }
        if (size == 0) return true;

        // Large one-off table loads are cheaper as one filesystem operation.
        // Runtime glyph access is normally far below this threshold.
        if (size >= block_bytes_ * 4U) {
            return source_->read(offset, data, size);
        }

        std::size_t copied = 0;
        while (copied < size) {
            const std::uint64_t absolute = offset + copied;
            const std::uint64_t block =
                absolute / block_bytes_;
            const std::size_t within = static_cast<std::size_t>(
                absolute % block_bytes_);
            Slot* slot = find_slot(block);
            if (slot == nullptr) {
                slot = fill_slot(block);
                if (slot == nullptr) return false;
            }
            const std::size_t available =
                slot->valid_bytes > within
                    ? slot->valid_bytes - within
                    : 0;
            if (available == 0) return false;
            const std::size_t count =
                std::min(size - copied, available);
            std::memcpy(
                data + copied,
                bytes_.data() + slot_index(*slot) * block_bytes_ + within,
                count);
            copied += count;
        }
        return true;
    }

private:
    struct Slot {
        std::uint64_t block =
            std::numeric_limits<std::uint64_t>::max();
        std::uint64_t age = 0;
        std::size_t valid_bytes = 0;
    };

    std::size_t slot_index(const Slot& slot) const {
        return static_cast<std::size_t>(&slot - slots_.data());
    }

    Slot* find_slot(std::uint64_t block) {
        for (Slot& slot : slots_) {
            if (slot.block == block) {
                slot.age = ++age_;
                return &slot;
            }
        }
        return nullptr;
    }

    Slot* fill_slot(std::uint64_t block) {
        Slot* victim = nullptr;
        for (Slot& slot : slots_) {
            if (slot.block ==
                std::numeric_limits<std::uint64_t>::max()) {
                victim = &slot;
                break;
            }
            if (victim == nullptr || slot.age < victim->age) {
                victim = &slot;
            }
        }
        if (victim == nullptr) return nullptr;

        const std::uint64_t begin = block * block_bytes_;
        if (begin >= source_->size()) return nullptr;
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                block_bytes_, source_->size() - begin));
        std::uint8_t* destination =
            bytes_.data() + slot_index(*victim) * block_bytes_;
        // A failing adapter is allowed to have partially written its output.
        // Retire the victim before the read so stale metadata can never expose
        // corrupted bytes as the old cached block.
        victim->block = std::numeric_limits<std::uint64_t>::max();
        victim->valid_bytes = 0;
        if (!source_->read(begin, destination, count)) {
            return nullptr;
        }
        victim->block = block;
        victim->valid_bytes = count;
        victim->age = ++age_;
        return victim;
    }

    std::shared_ptr<RandomAccessData> source_;
    std::size_t block_bytes_ = 0;
    std::vector<std::uint8_t> bytes_;
    std::vector<Slot> slots_;
    std::uint64_t age_ = 0;
};

inline std::shared_ptr<RandomAccessData> make_block_cached_random_access(
    std::shared_ptr<RandomAccessData> source,
    std::size_t cache_bytes = 32U * 1024U,
    std::size_t block_bytes = 1024U) {
    if (source == nullptr || source->contiguous_data() != nullptr) {
        return source;
    }
    try {
        return std::make_shared<BlockCachedRandomAccessData>(
            source, cache_bytes, block_bytes);
    } catch (const std::bad_alloc&) {
        // Font loading must remain functional under memory pressure; the
        // caller can still use the uncached stream with reduced performance.
        return source;
    }
}

}  // namespace nmarkdown

#endif
