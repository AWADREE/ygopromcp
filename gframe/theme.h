#ifndef YGOPRO_THEME_H
#define YGOPRO_THEME_H

#include <array>
#include <string>
#include <irrlicht.h>

namespace ygo {

struct ThemeVisualStyle {
	bool enabled = false;
	irr::video::SColor frame{0xffffffff};
	irr::video::SColor circuit{0};
	irr::video::SColor focus{0};
	irr::video::SColor error{0};
	irr::video::SColor backdrop{0};
	irr::video::SColor panel{0x400000ff};
	std::string backdropAsset;
	std::string logoAsset;
	int textSpacingX = 0;
	int textSpacingY = 0;
	int titleSpacingX = 0;
	bool customSpacing = false;
};

// Packages contain only validated JSON values and optional image/font files.
// The built-in Classic theme is the original Irrlicht skin captured at startup.
class ThemeManager {
public:
	static constexpr const char* ClassicId = "classic";
	~ThemeManager();
	ThemeManager() = default;
	ThemeManager(const ThemeManager&) = delete;
	ThemeManager& operator=(const ThemeManager&) = delete;
	bool Select(const std::string& id, irr::gui::IGUIEnvironment* environment,
		irr::video::IVideoDriver* driver, std::string* error = nullptr);
	bool IsAvailable(const std::string& id) const;
	const std::string& ActiveId() const { return activeId; }
	// The pile-count renderer owns placement and visibility; themes supply its text color.
	irr::video::SColor GetPileCountTextColor() const { return pileCountTextColor; }
	irr::video::SColor GetDeckPanelColor() const { return visual.enabled ? visual.panel : irr::video::SColor(0x400000ff); }
	irr::video::SColor GetDeckFrameColor() const { return visual.enabled ? visual.frame : irr::video::SColor(0xffffffff); }
	irr::video::SColor GetDeckTextColor() const { return visual.enabled ? activeTextColor : irr::video::SColor(0xffffffff); }
	irr::video::SColor GetFocusColor() const { return visual.enabled ? visual.focus : irr::video::SColor(0xffffffff); }
	irr::video::SColor GetCircuitColor() const { return visual.enabled ? visual.circuit : irr::video::SColor(0xffffffff); }
	bool HasVisualStyle() const { return visual.enabled; }
	void DrawBackdrop(irr::video::IVideoDriver* driver, const irr::core::recti& area) const;
	void DrawLogo(irr::video::IVideoDriver* driver, const irr::core::recti& area) const;
	void DrawChrome(irr::video::IVideoDriver* driver, irr::gui::IGUIEnvironment* environment) const;

private:
	void CaptureClassic(irr::gui::IGUISkin* skin);
	std::array<irr::video::SColor, irr::gui::EGDC_COUNT> classicColors{};
	std::array<irr::s32, 3> classicSpacing{};
	bool captured = false;
	irr::gui::IGUISkin* classicSkin = nullptr;
	irr::gui::IGUISkin* gildedSkin = nullptr;
	std::string activeId = ClassicId;
	irr::video::SColor pileCountTextColor{0xffffff00};
	irr::video::SColor activeTextColor{0xffffffff};
	ThemeVisualStyle visual{};
};

}

#endif
