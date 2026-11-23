#include "diagram_layout.hpp"

#include <algorithm>
#include <cmath>
#include <deque>

namespace drawio {
namespace {

std::vector<std::string> connectedOrder(const std::map<std::string, Size>& sizes, const std::vector<Edge>& edges) {
    std::map<std::string, std::set<std::string>> neighbors;
    for (const auto& [name, size] : sizes) {
        neighbors.try_emplace(name);
    }
    for (const auto& edge : edges) {
        neighbors.at(edge.source).insert(edge.target);
        neighbors.at(edge.target).insert(edge.source);
    }
    std::vector<std::string> order;
    std::set<std::string> seen;
    for (const auto& [name, adjacent] : neighbors) {
        if (!seen.insert(name).second) {
            continue;
        }
        std::deque<std::string> remaining{name};
        while (!remaining.empty()) {
            std::string next = std::move(remaining.front());
            remaining.pop_front();
            for (const auto& neighbor : neighbors.at(next)) {
                if (seen.insert(neighbor).second) {
                    remaining.push_back(neighbor);
                }
            }
            order.push_back(std::move(next));
        }
    }
    return order;
}

std::vector<int> starts(const std::vector<int>& sizes, int margin, int gap) {
    std::vector<int> result;
    for (int size : sizes) {
        result.push_back(margin);
        margin += size + gap;
    }
    return result;
}

}

DiagramLayout::DiagramLayout(const std::map<std::string, Size>& sizes, const std::vector<Edge>& edges, bool compact)
    : margin_(compact ? 54 : 80), columnGap_(compact ? 72 : 120), rowGap_(compact ? 62 : 108) {
    if (sizes.empty()) {
        return;
    }
    auto order = connectedOrder(sizes, edges);
    auto columns = std::min<std::size_t>(8, static_cast<std::size_t>(std::ceil(std::sqrt(order.size()))));
    auto rows = (order.size() + columns - 1) / columns;
    std::vector<int> widths(columns);
    std::vector<int> heights(rows);
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto size = sizes.at(order[i]);
        widths[i % columns] = std::max(widths[i % columns], size.width);
        heights[i / columns] = std::max(heights[i / columns], size.height);
    }
    auto columnStarts = starts(widths, margin_, columnGap_);
    rowTops_ = starts(heights, margin_, rowGap_);
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto row = i / columns;
        auto column = i % columns;
        boxes_.emplace(order[i], Box{{columnStarts[column], rowTops_[row]}, sizes.at(order[i]), row, column});
    }
}

const Box& DiagramLayout::box(const std::string& name) const {
    return boxes_.at(name);
}

Size DiagramLayout::bounds() const {
    Size result{};
    for (const auto& [name, box] : boxes_) {
        result.width = std::max(result.width, box.position.x + box.size.width + margin_);
        result.height = std::max(result.height, box.position.y + box.size.height + margin_);
    }
    return result;
}

int DiagramLayout::aboveRow(std::size_t row, int offset) const {
    return rowTops_.at(row) - rowGap_ / 2 + offset;
}

int DiagramLayout::leftOf(const Box& box, int offset) const {
    return box.position.x - columnGap_ / 2 + offset;
}

int laneOffset(const Edge& edge) {
    // Use a fixed hash so independent processes use the same corridors.
    unsigned hash = 0;
    for (auto name : {std::string_view(edge.source), std::string_view(edge.target), edgeName(edge.kind)}) {
        for (unsigned char ch : name) {
            hash = 31 * hash + ch;
        }
    }
    return static_cast<int>(hash % 7) * 6 - 18;
}

Route DiagramLayout::route(const Edge& edge, double lane) const {
    const auto& from = box(edge.source);
    const auto& to = box(edge.target);
    if (&from == &to) {
        int x = from.position.x + from.size.width + 28;
        return {{1, 0.28}, {1, 0.72}, {
            {x, from.position.y + static_cast<int>(std::round(from.size.height * 0.28))},
            {x, from.position.y + static_cast<int>(std::round(from.size.height * 0.72))}
        }};
    }
    bool sameRow = from.row == to.row;
    auto leftColumn = std::min(from.column, to.column);
    auto rightColumn = std::max(from.column, to.column);
    if (sameRow && rightColumn - leftColumn == 1) {
        double side = from.column < to.column ? 1 : 0;
        int fromY = from.position.y + static_cast<int>(std::round(from.size.height * lane));
        int toY = to.position.y + static_cast<int>(std::round(to.size.height * lane));
        int x = side == 1 ? (from.position.x + from.size.width + to.position.x) / 2
                          : (to.position.x + to.size.width + from.position.x) / 2;
        return {{side, lane}, {1 - side, lane}, {{x, fromY}, {x, toY}}};
    }

    bool down = from.row < to.row;
    bool up = from.row > to.row;
    int offset = laneOffset(edge);
    int fromY = aboveRow(from.row + down, offset);
    int toY = aboveRow(to.row + up, offset);
    int fromX = from.position.x + static_cast<int>(std::round(from.size.width * lane));
    int toX = to.position.x + static_cast<int>(std::round(to.size.width * lane));
    Route result{{lane, down ? 1.0 : 0.0}, {lane, up ? 1.0 : 0.0}, {{fromX, fromY}}};
    if (fromY != toY) {
        int corridor = leftOf(from, offset);
        result.bends.push_back({corridor, fromY});
        result.bends.push_back({corridor, toY});
    }
    result.bends.push_back({toX, toY});
    return result;
}

}
