#ifndef NMARKDOWN_LAYOUT_FENWICK_H
#define NMARKDOWN_LAYOUT_FENWICK_H

#include <cstddef>
#include <vector>

#include "nmarkdown/layout/fixed.h"

namespace nmarkdown {

class FenwickHeights {
public:
    void build(const std::vector<Fx>& values);
    void clear();
    std::size_t size() const { return values_.size(); }
    Fx value(std::size_t index) const;
    void update(std::size_t index, Fx value);
    void append(Fx value);
    Fx prefix_sum(std::size_t count) const;
    Fx total() const { return prefix_sum(size()); }

    // Returns the index containing vertical position target. If target is at
    // or beyond the total height, returns size().
    std::size_t lower_bound(Fx target) const;

private:
    std::vector<Fx> values_;
    std::vector<Fx> tree_;
};

}  // namespace nmarkdown

#endif
