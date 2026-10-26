#include "field_projection.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <utility>

struct Pixel { double x, y; };
struct Bounds { double left, right, top, bottom; };

static Pixel Project(double x, double y, double z, unsigned w, unsigned h,
                     const ygo::FieldProjection& frustum, ygo::FieldCamera camera) {
    const double distance = std::hypot(camera.y, camera.z);
    const double depth = distance - (y * camera.y + z * camera.z) / distance;
    const double vertical = (-y * camera.z + z * camera.y) / distance;
    return { ((x - 4.2) / depth - frustum.left) / frustum.Width() * w,
             (frustum.top - vertical / depth) / frustum.Height() * h };
}

static Bounds ProjectRect(double cx, double cy, double rectW, double rectH,
                          unsigned w, unsigned h, const ygo::FieldProjection& frustum,
                          ygo::FieldCamera camera) {
    Bounds bounds{1e9, -1e9, 1e9, -1e9};
    for(double sideX : {-1., 1.})
        for(double sideY : {-1., 1.}) {
            const auto p = Project(cx + sideX * rectW * .5,
                                   cy + sideY * rectH * .5, 0, w, h, frustum, camera);
            bounds.left = std::min(bounds.left, p.x);
            bounds.right = std::max(bounds.right, p.x);
            bounds.top = std::min(bounds.top, p.y);
            bounds.bottom = std::max(bounds.bottom, p.y);
        }
    return bounds;
}

static double Ratio(double cx, double cy, double cardW, double cardH, unsigned w, unsigned h,
                    const ygo::FieldProjection& frustum, ygo::FieldCamera camera) {
    const auto l = Project(cx - cardW / 2, cy, 0, w, h, frustum, camera);
    const auto r = Project(cx + cardW / 2, cy, 0, w, h, frustum, camera);
    const auto far = Project(cx, cy - cardH / 2, 0, w, h, frustum, camera);
    const auto near = Project(cx, cy + cardH / 2, 0, w, h, frustum, camera);
    return (r.x - l.x) / (near.y - far.y);
}

int main() {
    const unsigned sizes[][2] = {{1024,640},{1378,835},{2756,1670},{1920,1080},
                                 {2560,1440},{1280,960},{800,1280},{2048,1280}};
    for (bool modern : {false, true}) {
        const auto camera = ygo::GetFieldCamera(modern);
        double baseLocalCard = 0;
        for (const auto& size : sizes) {
            const auto w = size[0], h = size[1];
			const auto frustum = ygo::GetFieldProjectionForView(w, h, true, modern);
            // The screen ray must meet the same board point as the renderer for
            // both attack and defense card corners and both players' rows.
            for (double y : {-3.4, -1.4, 0.0, 1.4, 3.4}) {
                for (double x : {1.2, 4.2, 7.2}) {
                    const auto pixel = Project(x, y, 0, w, h, frustum, camera);
                    const auto hit = ygo::ScreenToBoard(pixel.x, pixel.y, w, h, frustum, camera);
                    assert(std::abs(hit.x - x) < 1e-5);
                    assert(std::abs(hit.y - y) < 1e-5);
                }
            }
            const auto localCard = Ratio(4.2, 1.4, .7, 1., w, h, frustum, camera);
            const auto farCard = Ratio(4.2, -1.4, .7, 1., w, h, frustum, camera);
            const auto localDefense = Ratio(4.2, 1.4, 1., .7, w, h, frustum, camera);
			const auto zone = Ratio(4.2, 1.4, modern ? ygo::MODERN_ZONE_WIDTH : 1.1,
				modern ? ygo::MODERN_ZONE_HEIGHT : 1.2, w, h, frustum, camera);
            if (!baseLocalCard) baseLocalCard = localCard;
            assert(std::abs(localCard - baseLocalCard) < 1e-5);
            assert(localCard < farCard);
            assert(localDefense > localCard);
			assert(zone > localCard);
			assert(zone < (modern ? 1.15 : 1.25));
            if (modern) {
                for(double rowY : {-1.4, 1.4}) {
                    const auto slot = ProjectRect(4.2, rowY, ygo::MODERN_ZONE_WIDTH,
                        ygo::MODERN_ZONE_HEIGHT, w, h, frustum, camera);
                    for(const auto cardSize : {std::pair<double,double>{.7,1.}, {1.,.7}}) {
                        const auto card = ProjectRect(4.2, rowY, cardSize.first,
                            cardSize.second, w, h, frustum, camera);
                        assert(slot.left < card.left && card.right < slot.right);
                        assert(slot.top < card.top && card.bottom < slot.bottom);
                    }
                }
                const double tilt = ygo::GetHandTilt(true);
                const double halfHeight = ygo::MODERN_HAND_SCALE * .5;
                const double zoneNearEdge = Project(4.2, 2.6 + ygo::MODERN_ZONE_HEIGHT * .5,
                    0, w, h, frustum, camera).y;
                for(bool hovered : {false, true}) {
                    // Hover changes outline only; the projected quad is fixed.
                    const double handY = ygo::MODERN_HAND_Y;
                    const double handZ = .5;
                    const auto edgeA = Project(4.2, handY + std::cos(tilt) * halfHeight,
                        handZ - std::sin(tilt) * halfHeight, w, h, frustum, camera);
                    const auto edgeB = Project(4.2, handY - std::cos(tilt) * halfHeight,
                        handZ + std::sin(tilt) * halfHeight, w, h, frustum, camera);
                    assert(std::min(edgeA.y, edgeB.y) > zoneNearEdge + 1.);
                    assert(std::max(edgeA.y, edgeB.y) < h);
                    for(int count : {1, 2, 5, 6, 7, 12, 20}) {
                        const double leftCenter = count <= 6
                            ? 4.2 - (count - 1) * .5 * ygo::MODERN_HAND_PITCH : 4.2 - 2.375;
                        const double rightCenter = count <= 6
                            ? 4.2 + (count - 1) * .5 * ygo::MODERN_HAND_PITCH : 4.2 + 2.375;
                        const auto left = Project(leftCenter - ygo::MODERN_HAND_SCALE * .35,
                            handY, handZ, w, h, frustum, camera);
                        const auto right = Project(rightCenter + ygo::MODERN_HAND_SCALE * .35,
                            handY, handZ, w, h, frustum, camera);
                        assert(left.x > 0 && right.x < w);
                    }
                }
                const auto handTop = Project(4.2, ygo::MODERN_HAND_Y + std::cos(tilt) * halfHeight,
                                             .5 - std::sin(tilt) * halfHeight, w, h, frustum, camera);
                const auto handBottom = Project(4.2, ygo::MODERN_HAND_Y - std::cos(tilt) * halfHeight,
                                                .5 + std::sin(tilt) * halfHeight, w, h, frustum, camera);
                const auto handLeft = Project(4.2 - 2.375 - ygo::MODERN_HAND_SCALE * .35,
                                              ygo::MODERN_HAND_Y, .5, w, h, frustum, camera);
                const auto handRight = Project(4.2 + 2.375 + ygo::MODERN_HAND_SCALE * .35,
                                               ygo::MODERN_HAND_Y, .5, w, h, frustum, camera);
                assert(std::min(handTop.y, handBottom.y) > 0 && std::max(handTop.y, handBottom.y) < h);
                assert(handLeft.x > 0 && handRight.x < w);
                assert(std::abs(handBottom.y - handTop.y) > 0);
                std::cout << "hand_bounds=" << handLeft.x << ',' << std::min(handTop.y, handBottom.y) << ".."
                          << handRight.x << ',' << std::max(handTop.y, handBottom.y) << ' ';
            }
            std::cout << (modern ? "modern" : "classic") << ' ' << w << 'x' << h
                      << " local_card=" << localCard << " far_card=" << farCard
                      << " defense=" << localDefense << " zone=" << zone << '\n';
        }
    }
	// The Modern visible zone covers the rotated card while leaving a gap.
	assert(ygo::MODERN_ZONE_WIDTH + 2 * ygo::MODERN_DEFENSE_PICK_MARGIN > 1.0);
	assert(ygo::MODERN_ZONE_WIDTH + 2 * ygo::MODERN_DEFENSE_PICK_MARGIN < 1.1);
    assert(1.0 < 1.1);
    assert(std::abs(ygo::GetHandTilt(true) - std::atan2(4.8,10.3)) < 1e-6);
}
