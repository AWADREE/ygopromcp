#ifndef FIELD_PROJECTION_H
#define FIELD_PROJECTION_H

#include <cmath>

namespace ygo {

// Leave room for a 1.0-wide Defense card and a 1.0-tall Attack card while
// preserving gaps between columns (pitch 1.1) and the existing row centers.
constexpr float MODERN_ZONE_WIDTH = 1.06f;
constexpr float MODERN_ZONE_HEIGHT = 1.05f;
constexpr float MODERN_DEFENSE_PICK_MARGIN = 0.0f;
constexpr float MODERN_HAND_SCALE = 1.15f;
constexpr float MODERN_HAND_PITCH = 0.95f;
// Keep local hand cards below the near Spell/Trap row, including hover lift.
constexpr float MODERN_HAND_Y = 4.1f;
// Measured 16:9 row-span match for the optional Modern view. Moving along
// the existing sightline preserves its pitch; scale the frustum reciprocally.
constexpr float MODERN_CAMERA_DISTANCE_SCALE = 1.65f;

struct FieldCamera {
	float y, z;
};

inline FieldCamera GetFieldCamera(bool modern) {
	return modern ? FieldCamera{ 4.8f * MODERN_CAMERA_DISTANCE_SCALE,
		10.3f * MODERN_CAMERA_DISTANCE_SCALE } : FieldCamera{ 8.0f, 7.8f };
}

inline float GetHandTilt(bool modern) {
	const auto camera = GetFieldCamera(modern);
	return std::atan2(camera.y, camera.z);
}

inline int NearestModernZoneColumn(double boardX, double firstCenter, double pitch) {
	int column = static_cast<int>(std::floor((boardX - firstCenter) / pitch + 0.5));
	if(column < 0) column = 0;
	if(column > 4) column = 4;
	return column;
}

// Keep the original framing at 1024x640 and reveal more of the board on the
// long axis when the window aspect changes. Equal world distances then occupy
// equal pixel distances on both screen axes before perspective is applied.
struct FieldProjection {
	float left, right, bottom, top;
	float Width() const { return right - left; }
	float Height() const { return top - bottom; }
	float ClassicX(float x) const { return 512.0f + (x - 512.0f) * Width() / 1.35f; }
	float ClassicY(float y) const { return 320.0f + (y - 320.0f) * Height() / 0.84f; }
};

inline FieldProjection GetFieldProjection(unsigned width, unsigned height, bool preserveAspect = true) {
	if(!preserveAspect)
		return { -0.90f, 0.45f, -0.42f, 0.42f };
	if(!width || !height)
		return { -0.90f, 0.45f, -0.42f, 0.42f };
	const float xUnitsPerPixel = 1.35f / width;
	const float yUnitsPerPixel = 0.84f / height;
	const float unitsPerPixel = xUnitsPerPixel > yUnitsPerPixel ? xUnitsPerPixel : yUnitsPerPixel;
	const float halfWidth = width * unitsPerPixel * 0.5f;
	const float halfHeight = height * unitsPerPixel * 0.5f;
	return { -0.225f - halfWidth, -0.225f + halfWidth, -halfHeight, halfHeight };
}

inline FieldProjection GetFieldProjectionForView(unsigned width, unsigned height,
	bool preserveAspect, bool modern) {
	auto projection = GetFieldProjection(width, height, preserveAspect);
	if(modern) {
		projection.left /= MODERN_CAMERA_DISTANCE_SCALE;
		projection.right /= MODERN_CAMERA_DISTANCE_SCALE;
		projection.bottom /= MODERN_CAMERA_DISTANCE_SCALE;
		projection.top /= MODERN_CAMERA_DISTANCE_SCALE;
	}
	return projection;
}

struct BoardPoint {
	double x, y;
};

// The screen point uses actual viewport pixels; its Y axis points downward,
// like Irrlicht's mouse coordinates. This inverts the same camera/frustum used
// by the renderer and intersects the ray with the board's Z=0 plane.
inline BoardPoint ScreenToBoard(double x, double y, unsigned width, unsigned height,
								const FieldProjection& projection, const FieldCamera& camera) {
	if(!width || !height)
		return { 0.0, 0.0 };
	const double sx = projection.left + x / width * projection.Width();
	const double sy = projection.bottom + y / height * projection.Height();
	const double angle = std::atan2(camera.y, camera.z) - std::atan(sy);
	const double boardY = camera.y - camera.z * std::tan(angle);
	const double boardX = 4.2 + camera.z * sx / (std::sqrt(1.0 + sy * sy) * std::cos(angle));
	return { boardX, boardY };
}

}
#endif
