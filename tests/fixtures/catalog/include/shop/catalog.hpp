#pragma once

#include <cstddef>

namespace stock {

struct Entry;

enum class Availability : unsigned { ready = 1, back_order = 4, retired };

struct Entry {
    int sku = 0;

    constexpr int code() const noexcept { return sku; }
};

class Identified {
protected:
    explicit Identified(int id) noexcept;

private:
    const int id_;
};

struct Readable {
    virtual ~Readable() = default;
    virtual const Entry& find(int sku) const & noexcept = 0;
};

} // namespace stock

namespace shop {

template <class T, std::size_t N = 4>
struct Shelf {
    T items[N];

    T& at(unsigned i) & noexcept { return items[i]; }

    template <typename U>
    U convert(U item) const { return item; }
};

class Order final : public stock::Identified, public virtual stock::Readable {
public:
    struct Audit {
        stock::Availability status;
    };

    Order(int id, const stock::Entry& entry) noexcept;
    explicit Order(const stock::Entry& entry) noexcept;
    const stock::Entry& find(int sku) const & noexcept override;
    int total(int price = 1) const;
    int total(double price) const;
    static int count;
    static constexpr int limit = 4;

private:
    Order* next_ = nullptr;
    const stock::Entry& entry_;
    Shelf<stock::Entry> copies_;
    unsigned cents_ : 12;
    int (*scorer_)(int) = nullptr;
};

} // namespace shop
