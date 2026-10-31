#include "nmarkdown/layout/fenwick.h"

namespace nmarkdown {

void FenwickHeights::clear() {
    values_.clear();
    tree_.clear();
}

void FenwickHeights::build(const std::vector<Fx>& values) {
    values_ = values;
    tree_.assign(values.size() + 1, 0);
    for (std::size_t index = 0; index < values.size(); ++index) {
        std::size_t cursor = index + 1;
        while (cursor < tree_.size()) {
            tree_[cursor] += values[index];
            cursor += cursor & (~cursor + 1);
        }
    }
}

Fx FenwickHeights::value(std::size_t index) const {
    return index < values_.size() ? values_[index] : 0;
}

void FenwickHeights::update(std::size_t index, Fx value) {
    if (index >= values_.size()) return;
    const Fx delta = value - values_[index];
    values_[index] = value;
    std::size_t cursor = index + 1;
    while (cursor < tree_.size()) {
        tree_[cursor] += delta;
        cursor += cursor & (~cursor + 1);
    }
}

void FenwickHeights::append(Fx value) {
    const std::size_t one_based = values_.size() + 1;
    const std::size_t range_begin = one_based - (one_based & (~one_based + 1));
    const Fx preceding = prefix_sum(one_based - 1) - prefix_sum(range_begin);
    values_.push_back(value);
    tree_.push_back(preceding + value);
}

Fx FenwickHeights::prefix_sum(std::size_t count) const {
    if (count > values_.size()) count = values_.size();
    Fx result = 0;
    std::size_t cursor = count;
    while (cursor > 0) {
        result += tree_[cursor];
        cursor -= cursor & (~cursor + 1);
    }
    return result;
}

std::size_t FenwickHeights::lower_bound(Fx target) const {
    if (target < 0) return 0;
    if (target >= total()) return size();

    std::size_t index = 0;
    Fx accumulated = 0;
    std::size_t step = 1;
    while ((step << 1U) < tree_.size()) step <<= 1U;
    for (; step != 0; step >>= 1U) {
        const std::size_t next = index + step;
        if (next < tree_.size() && accumulated + tree_[next] <= target) {
            index = next;
            accumulated += tree_[next];
        }
    }
    return index;
}

}  // namespace nmarkdown
