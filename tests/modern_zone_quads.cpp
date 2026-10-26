#include "field_projection.h"
#include "materials.h"

#include <matrix4.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

namespace {
using Quad = irr::video::S3DVertex[4];

double Width(const Quad& quad) {
	return std::abs(quad[1].Pos.X - quad[0].Pos.X);
}

double CenterX(const Quad& quad) {
	return (quad[0].Pos.X + quad[1].Pos.X) * 0.5;
}

double Height(const Quad& quad) {
	return std::abs(quad[2].Pos.Y - quad[0].Pos.Y);
}

double CenterY(const Quad& quad) {
	return (quad[0].Pos.Y + quad[2].Pos.Y) * 0.5;
}

bool SameQuad(const Quad& a, const Quad& b) {
	for(int i = 0; i < 4; ++i) {
		if(a[i].Pos != b[i].Pos || a[i].TCoords != b[i].TCoords)
			return false;
	}
	return true;
}

bool PickX(const Quad& quad, double x, double margin) {
	const double left = std::min(quad[0].Pos.X, quad[1].Pos.X);
	const double right = std::max(quad[0].Pos.X, quad[1].Pos.X);
	return x >= left - margin && x <= right + margin;
}

irr::core::matrix4 Projection(const ygo::FieldProjection& f) {
	irr::core::matrix4 m;
	for(int i = 0; i < 16; ++i) m[i] = 0;
	m[0] = 2.f / f.Width();
	m[5] = 2.f / f.Height();
	m[8] = (f.left + f.right) / (f.left - f.right);
	m[9] = (f.top + f.bottom) / (f.bottom - f.top);
	m[10] = 100.f / 99.f;
	m[11] = 1.f;
	m[14] = -100.f / 99.f;
	return m;
}

void CheckRoundTrip(const irr::core::matrix4& transform, const Quad& quad,
	unsigned width, unsigned height, const ygo::FieldProjection& f, ygo::FieldCamera camera) {
	for(int i = 0; i < 4; ++i) {
		float point[4] = {quad[i].Pos.X, quad[i].Pos.Y, quad[i].Pos.Z, 1.f};
		transform.multiplyWith1x4Matrix(point);
		assert(point[3] > 0);
		const double pixelX = width * 0.5 * (1. + point[0] / point[3]);
		const double pixelY = height * 0.5 * (1. - point[1] / point[3]);
		const auto board = ygo::ScreenToBoard(pixelX, pixelY, width, height, f, camera);
		assert(std::abs(board.x - quad[i].Pos.X) < 2e-5);
		assert(std::abs(board.y - quad[i].Pos.Y) < 2e-5);
	}
}
}

int main() {
	ygo::Materials classic;
	ygo::Materials modern;
	modern.UseModernZones();
	// The card face/back and their UVs are not modified to counter perspective.
	assert(SameQuad(classic.vCardFront, modern.vCardFront));
	assert(SameQuad(classic.vCardBack, modern.vCardBack));
	assert(std::abs(Width(modern.vCardFront) - .7) < 1e-6);
	for(int player = 0; player < 2; ++player) {
		for(int sequence = 0; sequence < 7; ++sequence) {
			const auto& before = classic.vFieldMzone[player][sequence];
			const auto& after = modern.vFieldMzone[player][sequence];
			assert(std::abs(CenterX(before) - CenterX(after)) < 1e-6);
			assert(std::abs(CenterY(before) - CenterY(after)) < 1e-6);
			assert(std::abs(Width(after) - ygo::MODERN_ZONE_WIDTH) < 1e-6);
			assert(std::abs(Height(after) - ygo::MODERN_ZONE_HEIGHT) < 1e-6);
		}
		for(int sequence = 0; sequence < 5; ++sequence)
			for(int rule = 0; rule < 2; ++rule) {
				const auto& before = classic.vFieldSzone[player][sequence][rule];
				const auto& after = modern.vFieldSzone[player][sequence][rule];
				assert(std::abs(CenterX(before) - CenterX(after)) < 1e-6);
				assert(std::abs(CenterY(before) - CenterY(after)) < 1e-6);
				assert(std::abs(Width(after) - ygo::MODERN_ZONE_WIDTH) < 1e-6);
				assert(std::abs(Height(after) - ygo::MODERN_ZONE_HEIGHT) < 1e-6);
			}
		assert(SameQuad(classic.vFieldDeck[player], modern.vFieldDeck[player]));
		assert(SameQuad(classic.vFieldExtra[player], modern.vFieldExtra[player]));
		for(int rule = 0; rule < 2; ++rule) {
			assert(SameQuad(classic.vFieldGrave[player][rule], modern.vFieldGrave[player][rule]));
			assert(SameQuad(classic.vFieldRemove[player][rule], modern.vFieldRemove[player][rule]));
			for(int sequence = 5; sequence < 8; ++sequence)
				assert(SameQuad(classic.vFieldSzone[player][sequence][rule], modern.vFieldSzone[player][sequence][rule]));
		}
		// Both card orientations fit within the visible zone, and adjacent
		// slots keep a pickable gap.
		const double pitch = std::abs(CenterX(modern.vFieldMzone[player][0]) - CenterX(modern.vFieldMzone[player][1]));
		assert(std::abs(pitch - 1.1) < 1e-6);
		assert(ygo::MODERN_ZONE_WIDTH + 2 * ygo::MODERN_DEFENSE_PICK_MARGIN < pitch);
		assert(ygo::MODERN_ZONE_WIDTH + 2 * ygo::MODERN_DEFENSE_PICK_MARGIN > 1.0);
		assert(ygo::MODERN_ZONE_HEIGHT > 1.0);
		const double firstCenter = CenterX(modern.vFieldMzone[0][0]);
		const double classicLeft = classic.vFieldMzone[0][0][0].Pos.X;
		for(int sequence = 0; sequence < 5; ++sequence) {
			const int expectedColumn = player == 0 ? sequence : 4 - sequence;
			for(double side : {-0.495, 0.495}) {
				const double x = CenterX(modern.vFieldMzone[player][sequence]) + side;
				assert(ygo::NearestModernZoneColumn(x, firstCenter, pitch) == expectedColumn);
				assert(PickX(modern.vFieldMzone[player][sequence], x, ygo::MODERN_DEFENSE_PICK_MARGIN));
				const double classicX = CenterX(classic.vFieldMzone[player][sequence]) + side;
				assert(static_cast<int>((classicX - classicLeft) / Width(classic.vFieldMzone[0][0])) == expectedColumn);
				assert(PickX(classic.vFieldMzone[player][sequence], classicX, 0));
			}
			for(int rule = 0; rule < 2; ++rule)
				for(double side : {-0.415, 0.415}) {
					const double x = CenterX(modern.vFieldSzone[player][sequence][rule]) + side;
					assert(ygo::NearestModernZoneColumn(x, firstCenter, pitch) == expectedColumn);
					assert(PickX(modern.vFieldSzone[player][sequence][rule], x, 0));
				}
		}
		for(int sequence = 5; sequence < 7; ++sequence) {
			const int expectedColumn = player == 0 ? (sequence == 5 ? 1 : 3) : (sequence == 5 ? 3 : 1);
			for(double side : {-0.495, 0.495}) {
				const double x = CenterX(modern.vFieldMzone[player][sequence]) + side;
				assert(ygo::NearestModernZoneColumn(x, firstCenter, pitch) == expectedColumn);
				assert(PickX(modern.vFieldMzone[player][sequence], x, ygo::MODERN_DEFENSE_PICK_MARGIN));
				const double classicX = CenterX(classic.vFieldMzone[player][sequence]) + side;
				assert(static_cast<int>((classicX - classicLeft) / Width(classic.vFieldMzone[0][0])) == expectedColumn);
				assert(PickX(classic.vFieldMzone[player][sequence], classicX, 0));
			}
		}
	}

	const unsigned sizes[][2] = {{1024,640},{1378,835},{2756,1670},{1920,1080},{2560,1440},{800,1280},{1280,960}};
	for(const auto& size : sizes) {
		const unsigned width = size[0], height = size[1];
		const auto f = ygo::GetFieldProjectionForView(width, height, true, true);
		const auto camera = ygo::GetFieldCamera(true);
		auto transform = Projection(f);
		irr::core::matrix4 view;
		view.buildCameraLookAtMatrixLH({4.2f,camera.y,camera.z}, {4.2f,0.f,0.f}, {0.f,0.f,1.f});
		transform *= view;
		for(int player = 0; player < 2; ++player) {
			for(int sequence = 0; sequence < 7; ++sequence)
				CheckRoundTrip(transform, modern.vFieldMzone[player][sequence], width, height, f, camera);
			for(int sequence = 0; sequence < 4; ++sequence) {
				const auto& a = modern.vFieldMzone[player][sequence];
				const auto& b = modern.vFieldMzone[player][sequence + 1];
				const double gapX = (CenterX(a) + CenterX(b)) * 0.5;
				const double gapY = (a[0].Pos.Y + a[2].Pos.Y) * 0.5;
				float point[4] = {static_cast<float>(gapX), static_cast<float>(gapY), 0.f, 1.f};
				transform.multiplyWith1x4Matrix(point);
				const auto board = ygo::ScreenToBoard(width * .5 * (1. + point[0] / point[3]),
					height * .5 * (1. - point[1] / point[3]), width, height, f, camera);
				assert(!PickX(a, board.x, ygo::MODERN_DEFENSE_PICK_MARGIN));
				assert(!PickX(b, board.x, ygo::MODERN_DEFENSE_PICK_MARGIN));
				assert(PickX(a, CenterX(a) + .5, ygo::MODERN_DEFENSE_PICK_MARGIN));
			}
			for(int rule = 0; rule < 2; ++rule)
				for(int sequence = 0; sequence < 8; ++sequence)
					CheckRoundTrip(transform, modern.vFieldSzone[player][sequence][rule], width, height, f, camera);
		}
		std::cout << width << 'x' << height << " quads and inverse projection pass\n";
	}
}
