#include "field_projection.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

struct Point { double x, y; };

static Point Project(double x, double y, unsigned width, unsigned height,
                     const ygo::FieldProjection& view) {
    constexpr double cameraY = 8.0;
    constexpr double cameraZ = 7.8;
    const double distance = std::hypot(cameraY, cameraZ);
    const double depth = distance - y * cameraY / distance;
    const double vertical = y * cameraZ / distance;
    return { ((x - 4.2) / depth - view.left) / view.Width() * width,
             (view.top - vertical / depth) / view.Height() * height };
}

static double CardRatio(unsigned width, unsigned height,
                        const ygo::FieldProjection& view) {
    const auto left = Project(4.2 - 0.35, -1.4, width, height, view);
    const auto right = Project(4.2 + 0.35, -1.4, width, height, view);
    const auto top = Project(4.2, -1.4 + 0.5, width, height, view);
    const auto bottom = Project(4.2, -1.4 - 0.5, width, height, view);
    return (right.x - left.x) / (bottom.y - top.y);
}

int main() {
    const unsigned sizes[][2] = { {1024, 640}, {1378, 835}, {2756, 1670},
                                  {1920, 1080}, {2560, 1440}, {1280, 960},
                                  {800, 1280}, {2048, 1280} };
    const double baseline = CardRatio(1024, 640, ygo::GetFieldProjection(1024, 640));
    for(const auto& size : sizes) {
        const auto width = size[0];
        const auto height = size[1];
        const auto view = ygo::GetFieldProjection(width, height);
        assert(std::abs(view.Width() / width - view.Height() / height) < 1e-8);
        assert(view.Width() >= 1.35 - 1e-6 && view.Height() >= 0.84 - 1e-6);
        assert(std::abs((view.left + view.right) * 0.5 + 0.225) < 1e-6);
        for(float classicX : { 340.0f, 512.0f, 760.0f }) {
            const float screenX = 512.0f + (classicX - 512.0f) * 1.35f / view.Width();
            assert(std::abs(view.ClassicX(screenX) - classicX) < 1e-3);
        }
        for(float classicY : { 160.0f, 320.0f, 540.0f }) {
            const float screenY = 320.0f + (classicY - 320.0f) * 0.84f / view.Height();
            assert(std::abs(view.ClassicY(screenY) - classicY) < 1e-3);
        }
        assert(std::abs(CardRatio(width, height, view) - baseline) < 1e-5);
        const float thumbScale = std::min(width / 1024.0f, height / 640.0f);
        const int thumbWidth = 44 * thumbScale;
        const int thumbHeight = 64 * thumbScale;
        assert(std::abs(double(thumbWidth) / thumbHeight - 44.0 / 64.0) < 0.02);
        std::cout << width << 'x' << height << " card=" << CardRatio(width, height, view)
                  << " thumb=" << double(thumbWidth) / thumbHeight << '\n';
    }
    // The legacy fixed frustum widens the same tilted field card at 16:9.
    const auto classic = ygo::GetFieldProjection(1920, 1080, false);
    assert(std::abs(classic.left + 0.90f) < 1e-6);
    assert(std::abs(classic.top - 0.42f) < 1e-6);
    assert(CardRatio(1920, 1080, classic) > baseline + 0.10);
}
