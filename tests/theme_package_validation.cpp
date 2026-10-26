#include "../gframe/theme.h"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static void make_dir(const char* path) {
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, 0700);
#endif
}

static void write_png(const std::string& path, bool truncated = false, bool badDeflate = false) {
	const char* hex = "89504e470d0a1a0a0000000d4948445200000001000000010802000000907753de"
		"0000000c49444154789c63606060000000040001f6173855"
		"0000000049454e44ae426082";
	std::vector<unsigned char> bytes;
	for(size_t i = 0; hex[i]; i += 2) {
		const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
		bytes.push_back(static_cast<unsigned char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
	}
	if(badDeflate) {
		bytes[41] = 0; // Invalid zlib header, but keep the PNG chunk checksum valid.
		const auto crc = crc32(0L, bytes.data() + 37, 16);
		for(int i = 0; i < 4; ++i)
			bytes[53 + i] = static_cast<unsigned char>(crc >> (24 - 8 * i));
	}
	if(truncated)
		bytes.resize(12);
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

static void write_manifest(const std::string& body) {
	std::ofstream out("themes/gilded/theme.json", std::ios::binary);
	out << body;
}

static std::string manifest(const std::string& assets = "{}", const std::string& pileCount = "",
	const std::string& additions = "") {
	std::string colors = "{";
	for(int i = 0; i < irr::gui::EGDC_COUNT; ++i) {
		if(i) colors += ',';
		colors += '"' + std::string(irr::gui::GUISkinColorNames[i]) + "\":\"#FF202020\"";
	}
	colors += '}';
	return "{\"schema_version\":1,\"id\":\"gilded\",\"name\":\"Test theme\",\"colors\":" + colors + ",\"assets\":" + assets +
		(pileCount.empty() ? "" : ",\"pile_count_text\":\"" + pileCount + "\"") + additions + "}";
}

int main(int argc, char** argv) {
	if(argc > 1 && std::string(argv[1]) == "existing") {
		ygo::ThemeManager installed;
		auto* device = irr::createDevice(irr::video::EDT_NULL, irr::core::dimension2d<irr::u32>(1, 1));
		if(!device || !installed.IsAvailable("gilded"))
			return 25;
		auto* skin = device->getGUIEnvironment()->getSkin();
		const auto classicSpacing = skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_X);
		if(!installed.Select("gilded", skin) || !installed.HasVisualStyle() ||
			installed.GetPileCountTextColor() != irr::video::SColor(0xfff2c96c) ||
			skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_X) != 6)
			return 26;
		if(!installed.Select("classic", skin) || installed.HasVisualStyle() ||
			skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_X) != classicSpacing)
			return 27;
		device->drop();
		std::cout << "installed Gilded package validation passed\n";
		return 0;
	}
	make_dir("themes");
	make_dir("themes/gilded");
	make_dir("themes/gilded/assets");
	make_dir("themes/gilded/assets/sub");
	make_dir("themes/other");
	make_dir("themes/outside");
	ygo::ThemeManager theme;
	write_manifest("{\"bad\":true}");
	if(!theme.IsAvailable("classic") || theme.IsAvailable("../classic") || theme.IsAvailable("gilded"))
		return 1;
	write_manifest(manifest());
	if(!theme.IsAvailable("gilded"))
		return 2;
	auto* device = irr::createDevice(irr::video::EDT_NULL, irr::core::dimension2d<irr::u32>(1, 1));
	if(!device)
		return 6;
	auto* skin = device->getGUIEnvironment()->getSkin();
	auto otherManifest = manifest();
	otherManifest.replace(otherManifest.find("\"gilded\""), 8, "\"other\"");
	{
		std::ofstream out("themes/other/theme.json", std::ios::binary);
		out << otherManifest;
	}
	if(theme.IsAvailable("other") || theme.Select("other", skin) || theme.ActiveId() != "classic")
		return 13;
	const auto classicColor = skin->getColor(irr::gui::EGDC_3D_DARK_SHADOW);
	const auto originalSize = skin->getSize(irr::gui::EGDS_SCROLLBAR_SIZE);
	const auto originalTextSpacing = skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_X);
	if(!theme.Select("gilded", skin) || theme.ActiveId() != "gilded" ||
		skin->getColor(irr::gui::EGDC_3D_DARK_SHADOW) != irr::video::SColor(0xff202020) ||
		theme.GetPileCountTextColor() != irr::video::SColor(0xffffff00))
		return 7;
	skin->setSize(irr::gui::EGDS_SCROLLBAR_SIZE, originalSize + 3);
	if(!theme.Select("classic", skin) || theme.ActiveId() != "classic" ||
		skin->getColor(irr::gui::EGDC_3D_DARK_SHADOW) != classicColor ||
		skin->getSize(irr::gui::EGDS_SCROLLBAR_SIZE) != originalSize + 3 ||
		theme.GetPileCountTextColor() != irr::video::SColor(0xffffff00))
		return 8;
	write_manifest(manifest("{}", "#FF11CCEE"));
	if(!theme.IsAvailable("gilded") || !theme.Select("gilded", skin) ||
		theme.GetPileCountTextColor() != irr::video::SColor(0xff11ccee))
		return 19;
	write_manifest(manifest("{}", "#GG11CCEE"));
	if(theme.IsAvailable("gilded") || theme.Select("gilded", skin) ||
		theme.ActiveId() != "classic" || theme.GetPileCountTextColor() != irr::video::SColor(0xffffff00))
		return 20;
	const std::string ui = ",\"ui\":{\"frame\":\"#FFE0B65B\",\"circuit\":\"#A89B793B\","
		"\"focus\":\"#FF10DDF7\",\"error\":\"#FFE56A60\",\"backdrop\":\"#B806121B\","
		"\"panel\":\"#EA0B1A24\"}";
	const std::string spacing = ",\"spacing\":{\"text_x\":6,\"text_y\":4,\"title_x\":12}";
	write_manifest(manifest("{}", "#FFF2C96C", ui + spacing));
	if(!theme.IsAvailable("gilded") || !theme.Select("gilded", skin) || !theme.HasVisualStyle() ||
		theme.GetDeckFrameColor() != irr::video::SColor(0xffe0b65b) ||
		theme.GetPileCountTextColor() != irr::video::SColor(0xfff2c96c) ||
		skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_X) != 6)
		return 21;
	if(!theme.Select("classic", skin) || theme.HasVisualStyle() ||
		skin->getSize(irr::gui::EGDS_TEXT_DISTANCE_X) != originalTextSpacing)
		return 22;
	write_manifest(manifest("{}", "", ui + ",\"spacing\":{\"text_x\":99,\"text_y\":4,\"title_x\":12}"));
	if(theme.IsAvailable("gilded") || theme.Select("gilded", skin) || theme.HasVisualStyle())
		return 23;
	write_manifest(manifest("{}", "", ",\"ui\":{\"frame\":\"#GG000000\"}"));
	if(theme.IsAvailable("gilded"))
		return 24;
	write_png("themes/gilded/assets/panel.png");
	write_manifest(manifest("{\"panel\":\"assets/panel.png\"}"));
	if(!theme.IsAvailable("gilded"))
		return 9;
	write_manifest(manifest("{\"backdrop\":\"assets/panel.png\"}"));
	if(theme.IsAvailable("gilded")) // A backdrop must be an exact 1920 x 1080 image.
		return 28;
	write_png("themes/gilded/assets/sub/panel.png");
	write_manifest(manifest("{\"panel\":\"assets/sub/panel.png\"}"));
	if(!theme.IsAvailable("gilded"))
		return 15;
	if(argc > 1 && std::string(argv[1]) == "outside-link") {
		write_png("themes/outside/panel.png");
		const char* link = "themes/gilded/assets/link";
#ifdef _WIN32
		const DWORD attributes = GetFileAttributesA(link);
		if(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
			return 17;
#else
		struct stat attributes{};
		if(lstat(link, &attributes) != 0 || !S_ISLNK(attributes.st_mode))
			return 17;
#endif
		if(!std::ifstream("themes/gilded/assets/link/panel.png", std::ios::binary))
			return 18;
		write_manifest(manifest("{\"panel\":\"assets/link/panel.png\"}"));
		if(theme.IsAvailable("gilded"))
			return 14;
	}
	write_manifest(manifest("{\"panel\":\"assets/panel.png\"}"));
	write_png("themes/gilded/assets/panel.png", false, true);
	if(theme.IsAvailable("gilded"))
		return 16;
	write_png("themes/gilded/assets/panel.png");
	{
		std::fstream png("themes/gilded/assets/panel.png", std::ios::binary | std::ios::in | std::ios::out);
		png.seekp(-1, std::ios::end);
		png.put(0); // Break the IEND checksum without changing the PNG signature.
	}
	if(theme.IsAvailable("gilded"))
		return 11;
	write_png("themes/gilded/assets/panel.png", true);
	if(theme.IsAvailable("gilded") || theme.Select("gilded", skin) || theme.ActiveId() != "classic" ||
		skin->getColor(irr::gui::EGDC_3D_DARK_SHADOW) != classicColor)
		return 10;
	write_manifest(manifest("{\"panel\":\"assets/missing.png\"}"));
	if(theme.IsAvailable("gilded"))
		return 3;
	write_manifest(manifest("{\"panel\":\"assets/../../script.lua\"}"));
	if(theme.IsAvailable("gilded"))
		return 4;
	write_manifest("{\"schema_version\":2,\"id\":\"gilded\",\"name\":\"Invalid\",\"colors\":{},\"assets\":{}}");
	if(theme.IsAvailable("gilded"))
		return 5;
	auto badColor = manifest();
	badColor.replace(badColor.find("#FF202020"), 9, "#GG202020");
	write_manifest(badColor);
	if(theme.IsAvailable("gilded"))
		return 12;
	device->drop();
	std::cout << "theme package validation passed\n";
	return 0;
}
