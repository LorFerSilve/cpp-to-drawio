#pragma once

#include "uml.hpp"

#include <map>

namespace drawio {

struct Size {
    int width;
    int height;
};

struct Point {
    int x;
    int y;
};

struct Box {
    Point position;
    Size size;
    std::size_t row;
    std::size_t column;
};

struct Port {
    double x;
    double y;
};

struct Route {
    Port exit;
    Port entry;
    std::vector<Point> bends;
};

class DiagramLayout {
public:
    DiagramLayout(const std::map<std::string, Size>& sizes, const std::vector<Edge>& edges, bool compact = false);

    const Box& box(const std::string& name) const;
    Size bounds() const;
    Route route(const Edge& edge, double lane) const;
    int aboveRow(std::size_t row, int offset) const;
    int leftOf(const Box& box, int offset) const;

private:
    int margin_;
    int columnGap_;
    int rowGap_;
    std::map<std::string, Box> boxes_;
    std::vector<int> rowTops_;
};

int laneOffset(const Edge& edge);

}
