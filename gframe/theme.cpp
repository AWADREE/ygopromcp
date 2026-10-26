#include "theme.h"
#include "game.h"
#include "../irrlicht/source/Irrlicht/CGUISkin.h"
#include "../vendor/nlohmann/json.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>
#include <png.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <cstdlib>
#endif

namespace ygo {
constexpr const char* ThemeManager::ClassicId;
namespace {
using json = nlohmann::json;

// The Irrlicht controls retain their native event, text, selection and keyboard
// behavior. Only their skin primitives change while Gilded is selected.
void RoundedFill(irr::video::IVideoDriver* driver, irr::core::recti area,
	irr::video::SColor color, const irr::core::recti* clip, int requestedRadius = 4) {
	if(!driver || area.getWidth() <= 0 || area.getHeight() <= 0)
		return;
	const int radius = std::min(requestedRadius,
		std::min(area.getWidth() / 2, area.getHeight() / 2));
	const int corner[] = {3, 1, 0, 0};
	for(int row = 0; row < radius; ++row) {
		const int inset = corner[std::min(row + 4 - radius, 3)];
		driver->draw2DRectangle(color, irr::core::recti(
			area.UpperLeftCorner.X + inset, area.UpperLeftCorner.Y + row,
			area.LowerRightCorner.X - inset, area.UpperLeftCorner.Y + row + 1), clip);
		driver->draw2DRectangle(color, irr::core::recti(
			area.UpperLeftCorner.X + inset, area.LowerRightCorner.Y - row - 1,
			area.LowerRightCorner.X - inset, area.LowerRightCorner.Y - row), clip);
	}
	if(area.getHeight() > radius * 2)
		driver->draw2DRectangle(color, irr::core::recti(
			area.UpperLeftCorner.X, area.UpperLeftCorner.Y + radius,
			area.LowerRightCorner.X, area.LowerRightCorner.Y - radius), clip);
}

void RoundedPanel(irr::video::IVideoDriver* driver, const irr::core::recti& area,
	irr::video::SColor fill, irr::video::SColor edge, const irr::core::recti* clip) {
	RoundedFill(driver, area, edge, clip);
	if(area.getWidth() > 2 && area.getHeight() > 2)
		RoundedFill(driver, irr::core::recti(area.UpperLeftCorner.X + 1,
			area.UpperLeftCorner.Y + 1, area.LowerRightCorner.X - 1,
			area.LowerRightCorner.Y - 1), fill, clip, 3);
}

void RoundedOutline(irr::video::IVideoDriver* driver, const irr::core::recti& area,
	irr::video::SColor color) {
	if(!driver || area.getWidth() < 9 || area.getHeight() < 9)
		return;
	const int x = area.UpperLeftCorner.X, y = area.UpperLeftCorner.Y;
	const int right = area.LowerRightCorner.X - 1, bottom = area.LowerRightCorner.Y - 1;
	driver->draw2DLine({x + 3, y}, {right - 3, y}, color);
	driver->draw2DLine({x + 3, bottom}, {right - 3, bottom}, color);
	driver->draw2DLine({x, y + 3}, {x, bottom - 3}, color);
	driver->draw2DLine({right, y + 3}, {right, bottom - 3}, color);
	for(int offset = 1; offset <= 2; ++offset) {
		driver->draw2DLine({x + offset, y + 3 - offset}, {x + offset, y + 3 - offset}, color);
		driver->draw2DLine({right - offset, y + 3 - offset}, {right - offset, y + 3 - offset}, color);
		driver->draw2DLine({x + offset, bottom - 3 + offset}, {x + offset, bottom - 3 + offset}, color);
		driver->draw2DLine({right - offset, bottom - 3 + offset}, {right - offset, bottom - 3 + offset}, color);
	}
}

class GildedSkin final : public irr::gui::CGUISkin {
public:
	GildedSkin(irr::gui::IGUISkin* source, irr::video::IVideoDriver* driver,
		irr::gui::IGUIEnvironment* environment, const ThemeVisualStyle* style)
		: CGUISkin(source->getType(), driver), driver(driver), environment(environment), style(style) {
		for(int i = 0; i < irr::gui::EGDC_COUNT; ++i)
			setColor(static_cast<irr::gui::EGUI_DEFAULT_COLOR>(i),
				source->getColor(static_cast<irr::gui::EGUI_DEFAULT_COLOR>(i)));
		for(int i = 0; i < irr::gui::EGDS_COUNT; ++i)
			setSize(static_cast<irr::gui::EGUI_DEFAULT_SIZE>(i),
				source->getSize(static_cast<irr::gui::EGUI_DEFAULT_SIZE>(i)));
		for(int i = 0; i < irr::gui::EGDF_COUNT; ++i)
			setFont(source->getFont(static_cast<irr::gui::EGUI_DEFAULT_FONT>(i)),
				static_cast<irr::gui::EGUI_DEFAULT_FONT>(i));
		setSpriteBank(source->getSpriteBank());
		for(int i = 0; i < irr::gui::EGDI_COUNT; ++i)
			setIcon(static_cast<irr::gui::EGUI_DEFAULT_ICON>(i),
				source->getIcon(static_cast<irr::gui::EGUI_DEFAULT_ICON>(i)));
		for(int i = 0; i < irr::gui::EGDT_COUNT; ++i)
			setDefaultText(static_cast<irr::gui::EGUI_DEFAULT_TEXT>(i),
				source->getDefaultText(static_cast<irr::gui::EGUI_DEFAULT_TEXT>(i)));
	}

	void draw3DButtonPaneStandard(irr::gui::IGUIElement* element,
		const irr::core::recti& area, const irr::core::recti* clip = nullptr) override {
		DrawButton(element, area, clip, false);
	}
	void draw3DButtonPanePressed(irr::gui::IGUIElement* element,
		const irr::core::recti& area, const irr::core::recti* clip = nullptr) override {
		DrawButton(element, area, clip, true);
	}
	void draw3DSunkenPane(irr::gui::IGUIElement* element, irr::video::SColor background,
		bool, bool fill, const irr::core::recti& area,
		const irr::core::recti* clip = nullptr) override {
		const bool enabled = !element || element->isEnabled();
		const bool focused = element && environment->hasFocus(element);
		const auto edge = !enabled ? getColor(irr::gui::EGDC_GRAY_TEXT) :
			focused ? style->focus : style->circuit;
		RoundedPanel(driver, area, fill ? background : getColor(irr::gui::EGDC_EDITABLE), edge, clip);
	}
	void draw3DMenuPane(irr::gui::IGUIElement*, const irr::core::recti& area,
		const irr::core::recti* clip = nullptr) override {
		RoundedPanel(driver, area, getColor(irr::gui::EGDC_WINDOW), style->frame, clip);
	}
	void draw3DToolBar(irr::gui::IGUIElement*, const irr::core::recti& area,
		const irr::core::recti* clip = nullptr) override {
		RoundedPanel(driver, area, style->panel, style->circuit, clip);
	}
	void draw3DTabButton(irr::gui::IGUIElement*, bool active,
		const irr::core::recti& area, const irr::core::recti* clip = nullptr,
		irr::gui::EGUI_ALIGNMENT = irr::gui::EGUIA_UPPERLEFT) override {
		RoundedPanel(driver, area, active ? getColor(irr::gui::EGDC_HIGH_LIGHT) : style->panel,
			active ? style->focus : style->circuit, clip);
	}
	void draw3DTabBody(irr::gui::IGUIElement*, bool border, bool background,
		const irr::core::recti& area, const irr::core::recti* clip = nullptr,
		irr::s32 tabHeight = -1,
		irr::gui::EGUI_ALIGNMENT alignment = irr::gui::EGUIA_UPPERLEFT) override {
		if(!background && !border)
			return;
		if(tabHeight < 0)
			tabHeight = getSize(irr::gui::EGDS_BUTTON_HEIGHT);
		auto body = area;
		if(alignment == irr::gui::EGUIA_UPPERLEFT)
			body.UpperLeftCorner.Y += tabHeight + 2;
		else
			body.LowerRightCorner.Y -= tabHeight + 2;
		if(body.getWidth() > 2 && body.getHeight() > 2)
			RoundedPanel(driver, body, background ? style->panel : irr::video::SColor(0),
				border ? style->circuit : irr::video::SColor(0), clip);
	}

private:
	void DrawButton(irr::gui::IGUIElement* element, const irr::core::recti& area,
		const irr::core::recti* clip, bool pressed) {
		const bool enabled = !element || element->isEnabled();
		const bool hovered = enabled && element && environment->getHovered() == element;
		const bool focused = enabled && element && environment->hasFocus(element);
		const int id = element ? element->getID() : -1;
		const bool destructive = id == BUTTON_MODE_EXIT || id == BUTTON_DELETE_REPLAY ||
			id == BUTTON_DELETE_DECK || id == BUTTON_DELETE_CATEGORY || id == BUTTON_DELETE_DECK_DM ||
			id == BUTTON_SURRENDER_YES;
		const bool primary = (id >= BUTTON_LAN_MODE && id <= BUTTON_DECK_EDIT) ||
			id == BUTTON_JOIN_HOST || id == BUTTON_CREATE_HOST || id == BUTTON_HOST_CONFIRM ||
			id == BUTTON_HP_START || id == BUTTON_LOAD_REPLAY || id == BUTTON_SAVE_DECK ||
			id == BUTTON_MCP_TOGGLE || id == BUTTON_START_FILTER;
		const auto edge = !enabled ? getColor(irr::gui::EGDC_GRAY_TEXT) :
			focused ? style->focus : destructive ? style->error :
			(hovered || primary ? style->frame : style->circuit);
		const auto base = !enabled ? getColor(irr::gui::EGDC_GRAY_EDITABLE) :
			pressed ? getColor(irr::gui::EGDC_HIGH_LIGHT) :
			hovered ? getColor(irr::gui::EGDC_FOCUSED_EDITABLE) :
			primary ? getColor(irr::gui::EGDC_3D_SHADOW) : getColor(irr::gui::EGDC_3D_FACE);
		RoundedPanel(driver, area, base, edge, clip);
	}
	irr::video::IVideoDriver* driver;
	irr::gui::IGUIEnvironment* environment;
	const ThemeVisualStyle* style;
};

bool ValidId(const std::string& id) {
	// The settings selector exposes only Classic and this one package.
	if(id != "gilded")
		return false;
	return true;
}

bool ValidAssetPath(const std::string& path) {
	if(path.size() < 12 || path.size() > 120 || path.compare(0, 7, "assets/") != 0 ||
		path.find("..") != std::string::npos || path.find('\\') != std::string::npos)
		return false;
	return std::all_of(path.begin(), path.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '.';
	});
}

bool CanonicalPath(const std::string& path, bool directory, std::string& result) {
#ifdef _WIN32
	const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
	HANDLE handle = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
	if(handle == INVALID_HANDLE_VALUE)
		return false;
	char resolved[32768]{};
	const DWORD size = GetFinalPathNameByHandleA(handle, resolved, sizeof resolved, FILE_NAME_NORMALIZED);
	CloseHandle(handle);
	if(size == 0 || size >= sizeof resolved)
		return false;
	result.assign(resolved, size);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
		return c == '/' ? '\\' : static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
	});
#else
	char resolved[PATH_MAX]{};
	if(!realpath(path.c_str(), resolved))
		return false;
	result = resolved;
#endif
	return true;
}

bool InsidePackage(const std::string& directory, const std::string& relative, std::string& resolvedAsset) {
	std::string resolvedDirectory;
	if(!CanonicalPath(directory, true, resolvedDirectory) ||
		!CanonicalPath(directory + relative, false, resolvedAsset))
		return false;
#ifdef _WIN32
	resolvedDirectory += '\\';
#else
	resolvedDirectory += '/';
#endif
	return resolvedAsset.compare(0, resolvedDirectory.size(), resolvedDirectory) == 0;
}

uint32_t BigEndian(const unsigned char* bytes) {
	return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
		(uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

bool CompletePngStructure(const std::vector<unsigned char>& data) {
	const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
	if(data.size() < 45 || !std::equal(std::begin(signature), std::end(signature), data.begin()))
		return false;
	bool sawImageData = false;
	size_t offset = 8;
	while(offset + 12 <= data.size()) {
		const auto length = BigEndian(data.data() + offset);
		if(length > data.size() - offset - 12)
			return false;
		const auto* type = data.data() + offset + 4;
		const auto* content = type + 4;
		const bool ihdr = std::equal(type, type + 4, "IHDR");
		const bool idat = std::equal(type, type + 4, "IDAT");
		const bool iend = std::equal(type, type + 4, "IEND");
		if(offset == 8 && (!ihdr || length != 13))
			return false;
		if(offset != 8 && ihdr)
			return false;
		uLong crc = crc32(0L, Z_NULL, 0);
		crc = crc32(crc, type, length + 4);
		if(crc != BigEndian(content + length))
			return false;
		if(idat)
			sawImageData = true;
		offset += 12 + length;
		if(iend)
			return length == 0 && sawImageData && offset == data.size();
	}
	return false;
}

bool ValidPng(const std::string& path, unsigned expectedWidth = 0, unsigned expectedHeight = 0) {
	if(path.size() < 4 || path.compare(path.size() - 4, 4, ".png") != 0)
		return false;
	std::ifstream file(path, std::ios::binary);
	if(!file)
		return false;
	file.seekg(0, std::ios::end);
	const auto bytes = file.tellg();
	if(bytes < 45 || bytes > 8 * 1024 * 1024)
		return false;
	file.seekg(0);
	std::vector<unsigned char> data(static_cast<size_t>(bytes));
	if(!file.read(reinterpret_cast<char*>(data.data()), data.size()))
		return false;
	if(!CompletePngStructure(data))
		return false;
	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	if(!png_image_begin_read_from_memory(&image, data.data(), data.size()))
		return false;
	if(image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096 ||
		(expectedWidth && image.width != expectedWidth) ||
		(expectedHeight && image.height != expectedHeight)) {
		png_image_free(&image);
		return false;
	}
	image.format = PNG_FORMAT_RGBA;
	std::vector<png_byte> pixels(PNG_IMAGE_SIZE(image));
	const bool decoded = png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) != 0;
	png_image_free(&image);
	return decoded;
}

bool HexColor(const std::string& value, irr::video::SColor& color) {
	if(value.size() != 9 || value[0] != '#')
		return false;
	unsigned int channels = 0;
	for(size_t i = 1; i < value.size(); ++i) {
		const unsigned char c = value[i];
		int digit = (c >= '0' && c <= '9') ? c - '0' :
			(c >= 'a' && c <= 'f') ? c - 'a' + 10 :
			(c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if(digit < 0)
			return false;
		channels = (channels << 4) | digit;
	}
	color = irr::video::SColor(channels);
	return true;
}

bool LoadPackage(const std::string& id,
	std::array<irr::video::SColor, irr::gui::EGDC_COUNT>& colors,
	irr::video::SColor& pileCountTextColor,
	ThemeVisualStyle& visual,
	std::string& error) {
	if(!ValidId(id)) { error = "Invalid theme identifier"; return false; }
	const std::string directory = "themes/" + id + "/";
	std::ifstream file(directory + "theme.json", std::ios::binary);
	if(!file) { error = "Theme package missing"; return false; }
	file.seekg(0, std::ios::end);
	const auto length = file.tellg();
	if(length <= 0 || length > 64 * 1024) { error = "Theme manifest size invalid"; return false; }
	file.seekg(0);
	json root = json::parse(file, nullptr, false);
	if(root.is_discarded() || !root.is_object() || root.size() < 5 || root.size() > 8 ||
		!root.contains("schema_version") || !root["schema_version"].is_number_integer() || root["schema_version"] != 1 ||
		!root.contains("id") || !root["id"].is_string() || root["id"] != id ||
		!root.contains("name") || !root["name"].is_string() ||
		!root.contains("colors") || !root["colors"].is_object() ||
		!root.contains("assets") || !root["assets"].is_object()) {
		error = "Theme manifest schema invalid"; return false;
	}
	for(auto it = root.begin(); it != root.end(); ++it) {
		const auto& key = it.key();
		if(key != "schema_version" && key != "id" && key != "name" && key != "colors" &&
			key != "assets" && key != "pile_count_text" && key != "ui" && key != "spacing") {
			error = "Unknown theme manifest key"; return false;
		}
	}
	const auto name = root["name"].get<std::string>();
	if(name.empty() || name.size() > 48 || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c > 126; })) {
		error = "Theme name invalid"; return false;
	}
	const auto& palette = root["colors"];
	if(palette.size() != irr::gui::EGDC_COUNT) { error = "Theme palette incomplete"; return false; }
	for(int i = 0; i < irr::gui::EGDC_COUNT; ++i) {
		const char* key = irr::gui::GUISkinColorNames[i];
		if(!palette.contains(key) || !palette[key].is_string() ||
			!HexColor(palette[key].get<std::string>(), colors[i])) {
			error = "Theme color invalid"; return false;
		}
	}
	pileCountTextColor = irr::video::SColor(0xffffff00);
	if(root.contains("pile_count_text") &&
		(!root["pile_count_text"].is_string() ||
		!HexColor(root["pile_count_text"].get<std::string>(), pileCountTextColor))) {
		error = "Pile count text color invalid"; return false;
	}
	visual = ThemeVisualStyle{};
	if(root.contains("ui")) {
		const auto& ui = root["ui"];
		if(!ui.is_object() || ui.size() != 6) { error = "Theme UI colors incomplete"; return false; }
		struct Entry { const char* name; irr::video::SColor* value; } entries[] = {
			{"frame", &visual.frame}, {"circuit", &visual.circuit}, {"focus", &visual.focus},
			{"error", &visual.error}, {"backdrop", &visual.backdrop}, {"panel", &visual.panel}
		};
		for(const auto& entry : entries) {
			if(!ui.contains(entry.name) || !ui[entry.name].is_string() ||
				!HexColor(ui[entry.name].get<std::string>(), *entry.value)) {
				error = "Theme UI color invalid"; return false;
			}
		}
		visual.enabled = true;
	}
	if(root.contains("spacing")) {
		const auto& spacing = root["spacing"];
		if(!spacing.is_object() || spacing.size() != 3) { error = "Theme spacing incomplete"; return false; }
		struct Entry { const char* name; int* value; } entries[] = {
			{"text_x", &visual.textSpacingX}, {"text_y", &visual.textSpacingY},
			{"title_x", &visual.titleSpacingX}
		};
		for(const auto& entry : entries) {
			if(!spacing.contains(entry.name) || !spacing[entry.name].is_number_integer()) {
				error = "Theme spacing invalid"; return false;
			}
			const auto value = spacing[entry.name].get<int>();
			if(value < 0 || value > 20) { error = "Theme spacing out of range"; return false; }
			*entry.value = value;
		}
		visual.customSpacing = true;
	}
	// Asset files are inert data. Paths stay inside this theme's own directory.
	const auto& assets = root["assets"];
	if(assets.size() > 16) { error = "Too many theme assets"; return false; }
	for(auto it = assets.begin(); it != assets.end(); ++it) {
		if(it.key().size() > 40 || !it.value().is_string()) { error = "Theme asset invalid"; return false; }
		const auto path = it.value().get<std::string>();
		std::string resolvedAsset;
		const bool backdrop = it.key() == "backdrop";
		const bool logo = it.key() == "logo";
		if(!backdrop && !logo) { error = "Unknown theme asset"; return false; }
		if(!ValidAssetPath(path) || !InsidePackage(directory, path, resolvedAsset) ||
			!ValidPng(resolvedAsset, backdrop ? 1920 : 2172, backdrop ? 1080 : 724)) {
			error = "Theme asset missing or invalid"; return false;
		}
		if(backdrop)
			visual.backdropAsset = resolvedAsset;
		else
			visual.logoAsset = resolvedAsset;
	}
	return true;
}
}

void ThemeManager::CaptureClassic(irr::gui::IGUISkin* skin) {
	if(captured || !skin)
		return;
	for(int i = 0; i < irr::gui::EGDC_COUNT; ++i)
		classicColors[i] = skin->getColor(static_cast<irr::gui::EGUI_DEFAULT_COLOR>(i));
	classicSpacing[0] = skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_X);
	classicSpacing[1] = skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_Y);
	classicSpacing[2] = skin->getSize(irr::gui::EGDS_TITLEBARTEXT_DISTANCE_X);
	captured = true;
}

ThemeManager::~ThemeManager() {
	if(gildedSkin) gildedSkin->drop();
	if(classicSkin) classicSkin->drop();
}

bool ThemeManager::Select(const std::string& id, irr::gui::IGUIEnvironment* environment,
	irr::video::IVideoDriver* driver, std::string* error) {
	if(!environment || !environment->getSkin() || !driver) {
		if(error) *error = "No GUI skin";
		return false;
	}
	if(!classicSkin) {
		classicSkin = environment->getSkin();
		classicSkin->grab();
	}
	CaptureClassic(classicSkin);
	if(id == ClassicId) {
		auto* skin = classicSkin;
		for(int i = 0; i < irr::gui::EGDC_COUNT; ++i)
			skin->setColor(static_cast<irr::gui::EGUI_DEFAULT_COLOR>(i), classicColors[i]);
		skin->setSize(irr::gui::EGDS_TEXT_DISTANCE_X, classicSpacing[0]);
		skin->setSize(irr::gui::EGDS_TEXT_DISTANCE_Y, classicSpacing[1]);
		skin->setSize(irr::gui::EGDS_TITLEBARTEXT_DISTANCE_X, classicSpacing[2]);
		activeId = ClassicId;
		pileCountTextColor = irr::video::SColor(0xffffff00);
		activeTextColor = irr::video::SColor(0xffffffff);
		visual = ThemeVisualStyle{};
		environment->setSkin(classicSkin);
		return true;
	}
	std::array<irr::video::SColor, irr::gui::EGDC_COUNT> colors{};
	irr::video::SColor countColor{0xffffff00};
	ThemeVisualStyle selectedVisual{};
	std::string failure;
	if(!LoadPackage(id, colors, countColor, selectedVisual, failure)) {
		Select(ClassicId, environment, driver);
		if(error) *error = failure;
		return false;
	}
	if(!gildedSkin)
		gildedSkin = new GildedSkin(classicSkin, driver, environment, &visual);
	auto* skin = gildedSkin;
	for(int i = 0; i < irr::gui::EGDC_COUNT; ++i)
		skin->setColor(static_cast<irr::gui::EGUI_DEFAULT_COLOR>(i), colors[i]);
	skin->setSize(irr::gui::EGDS_TEXT_DISTANCE_X,
		selectedVisual.customSpacing ? selectedVisual.textSpacingX : classicSpacing[0]);
	skin->setSize(irr::gui::EGDS_TEXT_DISTANCE_Y,
		selectedVisual.customSpacing ? selectedVisual.textSpacingY : classicSpacing[1]);
	skin->setSize(irr::gui::EGDS_TITLEBARTEXT_DISTANCE_X,
		selectedVisual.customSpacing ? selectedVisual.titleSpacingX : classicSpacing[2]);
	activeId = id;
	pileCountTextColor = countColor;
	activeTextColor = colors[irr::gui::EGDC_BUTTON_TEXT];
	visual = selectedVisual;
	environment->setSkin(gildedSkin);
	return true;
}

bool ThemeManager::IsAvailable(const std::string& id) const {
	if(id == ClassicId)
		return true;
	std::array<irr::video::SColor, irr::gui::EGDC_COUNT> colors{};
	irr::video::SColor countColor{0xffffff00};
	ThemeVisualStyle selectedVisual{};
	std::string error;
	return LoadPackage(id, colors, countColor, selectedVisual, error);
}

void ThemeManager::DrawBackdrop(irr::video::IVideoDriver* driver, const irr::core::recti& area) const {
	if(!visual.enabled || !driver)
		return;
	driver->draw2DRectangle(visual.backdrop, area);
	const auto left = area.UpperLeftCorner.X;
	const auto top = area.UpperLeftCorner.Y;
	const auto right = area.LowerRightCorner.X;
	const auto bottom = area.LowerRightCorner.Y;
	if(!visual.backdropAsset.empty()) {
		auto* image = driver->getTexture(visual.backdropAsset.c_str());
		if(image) {
			const auto size = image->getOriginalSize();
			const float scale = std::min(float(area.getWidth()) / size.Width,
				float(area.getHeight()) / size.Height);
			const int width = int(size.Width * scale);
			const int height = int(size.Height * scale);
			const int x = left + (area.getWidth() - width) / 2;
			const int y = top + (area.getHeight() - height) / 2;
			driver->draw2DImage(image, irr::core::recti(x, y, x + width, y + height),
				irr::core::recti(0, 0, size.Width, size.Height), nullptr, nullptr, false);
		}
	}
	// The procedural frame also serves as a complete fallback before artwork is installed.
	driver->draw2DRectangleOutline(irr::core::recti(left + 8, top + 8, right - 8, bottom - 8), visual.circuit);
	driver->draw2DLine({left + 32, top + 90}, {right - 32, top + 90}, visual.circuit);
	driver->draw2DLine({left + 32, bottom - 64}, {right - 32, bottom - 64}, visual.circuit);
	for(int i = 0; i < 3; ++i) {
		const int inset = 16 + i * 14;
		driver->draw2DLine({left + inset, top + 2}, {left + inset, top + 52 + i * 8}, visual.circuit);
		driver->draw2DLine({left + inset, top + 52 + i * 8},
			{left + inset + 24, top + 76 + i * 8}, visual.circuit);
		driver->draw2DLine({left + inset + 24, top + 76 + i * 8},
			{left + inset + 112, top + 76 + i * 8}, visual.circuit);
		driver->draw2DLine({right - inset - 112, bottom - 76 - i * 8},
			{right - inset - 24, bottom - 76 - i * 8}, visual.circuit);
		driver->draw2DLine({right - inset - 24, bottom - 76 - i * 8},
			{right - inset, bottom - 52 - i * 8}, visual.circuit);
		driver->draw2DLine({right - inset, bottom - 52 - i * 8},
			{right - inset, bottom - 2}, visual.circuit);
	}
}

void ThemeManager::DrawLogo(irr::video::IVideoDriver* driver, const irr::core::recti& area) const {
	if(!visual.enabled || !driver || visual.logoAsset.empty() ||
		area.getWidth() <= 0 || area.getHeight() <= 0) return;
	auto* image = driver->getTexture(visual.logoAsset.c_str());
	if(!image || image->getOriginalSize() != irr::core::dimension2du(2172, 724)) return;
	// Losslessly use the approved source pixels. The transparent source canvas
	// contains a 2168 x 311 visible wordmark; no screenshot or art is cut out.
	const irr::core::recti source(3, 203, 2171, 514);
	const float scale = std::min(float(area.getWidth()) / source.getWidth(),
		float(area.getHeight()) / source.getHeight());
	const int width = int(source.getWidth() * scale);
	const int height = int(source.getHeight() * scale);
	const int x = area.UpperLeftCorner.X + (area.getWidth() - width) / 2;
	const int y = area.UpperLeftCorner.Y + (area.getHeight() - height) / 2;
	driver->draw2DImage(image, irr::core::recti(x, y, x + width, y + height),
		source, nullptr, nullptr, true);
}

namespace {
void DrawWindowChrome(irr::video::IVideoDriver* driver, irr::gui::IGUIElement* parent,
	const ThemeVisualStyle& style) {
	if(!parent || !parent->isVisible())
		return;
	for(auto it = parent->getChildren().begin(); it != parent->getChildren().end(); ++it) {
		auto* element = *it;
		if(!element->isVisible())
			continue;
		const auto type = element->getType();
		if(type == irr::gui::EGUIET_WINDOW || type == irr::gui::EGUIET_MESSAGE_BOX) {
			const auto area = element->getAbsolutePosition();
			if(area.getWidth() >= 100 && area.getHeight() >= 50) {
				const auto edge = type == irr::gui::EGUIET_MESSAGE_BOX ? style.error : style.frame;
				driver->draw2DRectangleOutline(area, edge);
				const int left = area.UpperLeftCorner.X;
				const int top = area.UpperLeftCorner.Y;
				const int right = area.LowerRightCorner.X;
				driver->draw2DLine({left + 9, top + 3}, {left + 42, top + 3}, style.circuit);
				driver->draw2DLine({left + 42, top + 3}, {left + 49, top + 10}, style.circuit);
				driver->draw2DLine({left + 49, top + 10}, {right - 9, top + 10}, style.circuit);
			}
		}
		DrawWindowChrome(driver, element, style);
	}
}
}

void ThemeManager::DrawChrome(irr::video::IVideoDriver* driver,
	irr::gui::IGUIEnvironment* environment) const {
	if(!visual.enabled || !driver || !environment)
		return;
	DrawWindowChrome(driver, environment->getRootGUIElement(), visual);
	auto* focus = environment->getFocus();
	if(!focus || !focus->isVisible() || !focus->isEnabled())
		return;
	for(auto* ancestor = focus->getParent(); ancestor; ancestor = ancestor->getParent()) {
		if(!ancestor->isVisible() || !ancestor->isEnabled())
			return;
	}
	const auto type = focus->getType();
	if(type == irr::gui::EGUIET_BUTTON || type == irr::gui::EGUIET_EDIT_BOX ||
		type == irr::gui::EGUIET_COMBO_BOX || type == irr::gui::EGUIET_LIST_BOX ||
		type == irr::gui::EGUIET_CHECK_BOX || type == irr::gui::EGUIET_TAB_CONTROL)
		RoundedOutline(driver, focus->getAbsolutePosition(), visual.focus);
}
}
