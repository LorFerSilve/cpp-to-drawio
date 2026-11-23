#pragma once

#if defined(WITH_AUDIT) && WITH_AUDIT == 1
#include <vector>

namespace audit {
struct Trail {
    std::vector<stock::Entry> changes;
    shop::Order* item;
    [[nodiscard]] stock::Readable* source() const;
};
}
#else
#error Incorrect compile flags for audit.cpp
#endif
