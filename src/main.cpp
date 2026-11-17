#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <sstream>
#include <vector>

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
	inline size_t idx = 0;
	inline bool injecting = false;

	inline int frame() {
		auto pl = PlayLayer::get();
		return pl ? pl->m_gameState.m_currentProgress : 0;
	}

	inline auto file() {
		return Mod::get()->getSaveDir() / "macro.txt";
	}

	inline void save() {
		std::ostringstream out;
		for (auto& i : macro) {
			out << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0) << '\n';
		}
		(void)utils::file::writeString(file(), out.str());
	}

	inline bool load() {
		auto res = utils::file::readString(file());
		if (res.isErr()) return false;
		std::istringstream in(res.unwrap());
		macro.clear();
		int f, b, d, p;
		while (in >> f >> b >> d >> p) {
			macro.push_back({f, b, d != 0, p != 0});
		}
		return !macro.empty();
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
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}
};

class $modify(RBPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		RB::idx = 0;
		if (RB::mode == Mode::Recording) RB::macro.clear();
	}

	void levelComplete() {
		if (RB::mode == Mode::Recording) {
			RB::save();
			Notification::create("Macro saved", NotificationIcon::Success)->show();
		}
		RB::mode = Mode::Idle;
		PlayLayer::levelComplete();
	}

	void onQuit() {
		if (RB::mode == Mode::Recording && !RB::macro.empty()) RB::save();
		RB::mode = Mode::Idle;
		PlayLayer::onQuit();
	}
};

class $modify(RBPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto win = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		menu->setPosition({win.width - 55.f, win.height / 2});

		auto mk = [&](char const* text, SEL_MenuHandler h, float y) {
			auto spr = ButtonSprite::create(text, 70, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.6f);
			auto btn = CCMenuItemSpriteExtra::create(spr, this, h);
			btn->setPosition({0.f, y});
			menu->addChild(btn);
		};

		mk("Record", menu_selector(RBPause::onRec), 40.f);
		mk("Replay", menu_selector(RBPause::onPlay), 0.f);
		mk("Stop", menu_selector(RBPause::onStop), -40.f);

		this->addChild(menu);
	}

	void onRec(CCObject*) {
		RB::macro.clear();
		RB::mode = Mode::Recording;
		Notification::create("Recording...", NotificationIcon::Info)->show();
		this->onRestart(nullptr);
	}

	void onPlay(CCObject*) {
		if (RB::macro.empty() && !RB::load()) {
			Notification::create("No macro found", NotificationIcon::Error)->show();
			return;
		}
		RB::mode = Mode::Replaying;
		Notification::create("Replaying...", NotificationIcon::Info)->show();
		this->onRestart(nullptr);
	}

	void onStop(CCObject*) {
		if (RB::mode == Mode::Recording && !RB::macro.empty()) {
			RB::save();
			Notification::create("Macro saved", NotificationIcon::Success)->show();
		}
		RB::mode = Mode::Idle;
	}
};
