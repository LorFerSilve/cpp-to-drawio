#include "shop/catalog.hpp"

#if defined(WITH_AUDIT) || __cplusplus != 201703L
#error Incorrect compile flags for order.cpp
#endif

stock::Identified::Identified(int number) noexcept : id_(number) {}

shop::Order::Order(int number, const stock::Entry& original) noexcept
    : stock::Identified(number), entry_(original) {}

shop::Order::Order(const stock::Entry& original) noexcept : Order(original.sku, original) {}

const stock::Entry& shop::Order::find(int) const & noexcept { return entry_; }
int shop::Order::total(int price) const { return price; }
int shop::Order::total(double price) const { return static_cast<int>(price); }
int shop::Order::count = 0;

struct GlobalRecord {};
