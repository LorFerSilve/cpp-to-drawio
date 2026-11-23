#include "shop/catalog.hpp"
#include "options.hpp"

#if __cplusplus < 202002L
#error audit.cpp requires C++20
#endif

stock::Readable* audit::Trail::source() const { return item; }
