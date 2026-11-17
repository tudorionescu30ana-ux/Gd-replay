#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <sstream>
#include <vector>
#include <string>

using namespace geode::prelude;

enum class Mode { Idle, Recording, Replaying };

struct Input {
	int frame;
	int button;
	bool down;
	bool p1;
};

namespace RB {
	inline Mode mode = Mode::Idle;
	inline std::vector<Input> macro;
	inline std::vector<Input> prev;      // last attempt while recording
	inline size_t idx = 0;
	inline int injected = 0;
	inline bool injecting = false;

	inline std::vector<std::string> names; // saved macro names
	inline int selected = -1;
	inline bool indexLoaded = false;

	inline int frame() {
		auto pl = PlayLayer::get();
		return pl ? pl->m_gameState.m_currentProgress : 0;
	}

	inline auto pathOf(std::string const& name) {
		std::string f = name + ".txt";
		return Mod::get()->getSaveDir() / f.c_str();
	}

	inline std::string serialize(std::vector<Input> const& m) {
		std::ostringstream out;
		for (auto& i : m) {
			out << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0) << '\n';
		}
		return out.str();
	}

	inline std::vector<Input> parse(std::string const& text) {
		std::vector<Input> m;
		std::istringstream in(text);
		int f, b, d, p;
		while (in >> f >> b >> d >> p) {
			m.push_back({f, b, d != 0, p != 0});
		}
		return m;
	}

	inline void saveIndex() {
		std::string t;
		for (auto& n : names) t += n + "\n";
		(void)utils::file::writeString(Mod::get()->getSaveDir() / "index.txt", t);
	}

	inline void loadIndex() {
		if (indexLoaded) return;
		indexLoaded = true;
		auto res = utils::file::readString(Mod::get()->getSaveDir() / "index.txt");
		if (res.isErr()) return;
		std::istringstream in(res.unwrap());
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (!line.empty()) names.push_back(line);
		}
	}

	inline void addName(std::string const& n) {
		for (size_t i = 0; i < names.size(); i++) {
			if (names[i] == n) { selected = (int)i; return; }
		}
		names.push_back(n);
		selected = (int)names.size() - 1;
		saveIndex();
	}

	// write macro under a given name (overwrites)
	inline void saveAs(std::string const& name, std::vector<Input> const& m) {
		loadIndex();
		(void)utils::file::writeString(pathOf(name), serialize(m));
		addName(name);
	}

	inline std::string newName(std::string const& base) {
		loadIndex();
		for (int i = 1;; i++) {
			std::string n = base + "_" + std::to_string(i);
			bool used = false;
			for (auto& x : names) if (x == n) { used = true; break; }
			if (!used) return n;
		}
	}

	inline bool loadByName(std::string const& name) {
		auto res = utils::file::readString(pathOf(name));
		if (res.isErr()) return false;
		auto m = parse(res.unwrap());
		if (m.empty()) return false;
		macro = m;
		return true;
	}

	// autosave of the current recording (overwritten every time)
	inline void autosave() {
		if (macro.empty()) return;
		saveAs("last", macro);
	}
}

class $modify(RBGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		bool inPlay = this == static_cast<GJBaseGameLayer*>(PlayLayer::get());

		if (inPlay) {
			// while replaying, ignore real player input
			if (RB::mode == Mode::Replaying && !RB::injecting) return;

			if (RB::mode == Mode::Recording) {
				RB::macro.push_back({RB::frame(), button, down, isPlayer1});
				log::info("rec frame={} btn={} down={}", RB::frame(), button, down);
			}
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (RB::mode == Mode::Replaying && this == static_cast<GJBaseGameLayer*>(PlayLayer::get())) {
			int f = RB::frame();
			while (RB::idx < RB::macro.size() && RB::macro[RB::idx].frame <= f) {
				auto& i = RB::macro[RB::idx];
				RB::injecting = true;
				this->handleButton(i.down, i.button, i.p1);
				RB::injecting = false;
				RB::idx++;
				RB::injected++;
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}
};

class $modify(RBPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		RB::idx = 0;
		RB::injected = 0;
		if (RB::mode == Mode::Recording) {
			if (!RB::macro.empty()) RB::prev = RB::macro;
			RB::macro.clear();
		}
	}

	void levelComplete() {
		if (RB::mode == Mode::Recording) {
			RB::autosave();
			Notification::create("Macro finished - press Save to keep it", NotificationIcon::Success)->show();
		}
		RB::mode = Mode::Idle;
		PlayLayer::levelComplete();
	}

	void onQuit() {
		if (RB::mode == Mode::Recording) RB::autosave();
		RB::mode = Mode::Idle;
		PlayLayer::onQuit();
	}
};

class $modify(RBPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		RB::loadIndex();

		auto win = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		menu->setPosition({win.width - 55.f, win.height / 2});

		auto mk = [&](char const* text, SEL_MenuHandler h, float y) {
			auto spr = ButtonSprite::create(text, 70, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.6f);
			auto btn = CCMenuItemSpriteExtra::create(spr, this, h);
			btn->setPosition({0.f, y});
			menu->addChild(btn);
		};

		mk("Record", menu_selector(RBPause::onRec), 85.f);
		mk("Replay", menu_selector(RBPause::onPlay), 51.f);
		mk("Stop", menu_selector(RBPause::onStop), 17.f);
		mk("Save", menu_selector(RBPause::onSave), -17.f);
		mk("Select", menu_selector(RBPause::onSelect), -51.f);
		mk("Import", menu_selector(RBPause::onImport), -85.f);

		this->addChild(menu);
	}

	void onRec(CCObject*) {
		RB::macro.clear();
		RB::prev.clear();
		RB::mode = Mode::Recording;
		Notification::create("Recording...", NotificationIcon::Info)->show();
		this->onRestart(nullptr);
	}

	void onPlay(CCObject*) {
		if (RB::macro.empty()) {
			RB::loadIndex();
			bool ok = false;
			if (RB::selected >= 0 && RB::selected < (int)RB::names.size()) ok = RB::loadByName(RB::names[RB::selected]);
			if (!ok) ok = RB::loadByName("last");
			if (!ok) {
				Notification::create("No macro found", NotificationIcon::Error)->show();
				return;
			}
		}
		RB::mode = Mode::Replaying;
		Notification::create(fmt::format("Replaying {} inputs", RB::macro.size()), NotificationIcon::Info)->show();
		this->onRestart(nullptr);
	}

	void onStop(CCObject*) {
		if (RB::mode == Mode::Recording) {
			if (RB::macro.empty()) RB::macro = RB::prev;
			if (!RB::macro.empty()) {
				RB::autosave();
				Notification::create(fmt::format("Stopped: {} inputs (press Save)", RB::macro.size()), NotificationIcon::Success)->show();
			} else {
				Notification::create("0 inputs recorded", NotificationIcon::Error)->show();
			}
		} else if (RB::mode == Mode::Replaying) {
			Notification::create(fmt::format("Injected {}/{}", RB::injected, RB::macro.size()), NotificationIcon::Info)->show();
		}
		RB::mode = Mode::Idle;
	}

	void onSave(CCObject*) {
		if (RB::mode == Mode::Recording && RB::macro.empty()) RB::macro = RB::prev;
		if (RB::macro.empty()) {
			Notification::create("Nothing to save", NotificationIcon::Error)->show();
			return;
		}
		auto name = RB::newName("macro");
		RB::saveAs(name, RB::macro);
		Notification::create(fmt::format("Saved as {} ({} inputs)", name, RB::macro.size()), NotificationIcon::Success)->show();
	}

	void onSelect(CCObject*) {
		RB::loadIndex();
		if (RB::names.empty()) {
			Notification::create("No saved macros", NotificationIcon::Error)->show();
			return;
		}
		RB::selected = (RB::selected + 1) % (int)RB::names.size();
		auto name = RB::names[RB::selected];
		if (RB::loadByName(name)) {
			Notification::create(fmt::format("Selected {} ({}/{}) - {} inputs", name, RB::selected + 1, RB::names.size(), RB::macro.size()), NotificationIcon::Success)->show();
		} else {
			Notification::create(fmt::format("Can't load {}", name), NotificationIcon::Error)->show();
		}
	}

	void onImport(CCObject*) {
		auto text = utils::clipboard::read();
		auto m = RB::parse(text);
		if (m.empty()) {
			Notification::create("Clipboard has no macro", NotificationIcon::Error)->show();
			return;
		}
		RB::macro = m;
		auto name = RB::newName("import");
		RB::saveAs(name, m);
		Notification::create(fmt::format("Imported {} ({} inputs)", name, m.size()), NotificationIcon::Success)->show();
	}
};
