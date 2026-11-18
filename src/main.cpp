#include <Geode/Geode.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
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
	inline std::vector<Input> prev;
	inline size_t idx = 0;
	inline int injected = 0;
	inline bool injecting = false;
	inline bool deathSaved = false;

	inline std::vector<std::string> names;
	inline int selected = -1;
	inline bool indexLoaded = false;

	inline void notify(std::string const& t, NotificationIcon icon = NotificationIcon::Info) {
		Notification::create(t, icon)->show();
	}

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

	// own text format: "frame button down p1" per line
	inline std::vector<Input> parseText(std::string const& text) {
		std::vector<Input> m;
		std::istringstream in(text);
		int f, b, d, p;
		while (in >> f >> b >> d >> p) {
			m.push_back({f, b, d != 0, p != 0});
		}
		return m;
	}

	inline bool getField(std::string const& c, char const* key, double& out) {
		std::string k = std::string("\"") + key + "\"";
		auto p = c.find(k);
		if (p == std::string::npos) return false;
		p = c.find(':', p + k.size());
		if (p == std::string::npos) return false;
		p++;
		while (p < c.size() && isspace((unsigned char)c[p])) p++;
		if (c.compare(p, 4, "true") == 0) { out = 1; return true; }
		if (c.compare(p, 5, "false") == 0) { out = 0; return true; }
		char* end = nullptr;
		out = strtod(c.c_str() + p, &end);
		return end != c.c_str() + p;
	}

	// GDR-style JSON: objects with "frame", "btn", "2p", "down"
	inline std::vector<Input> parseJson(std::string const& t) {
		std::vector<Input> m;
		size_t pos = 0;
		while (true) {
			auto open = t.find('{', pos);
			if (open == std::string::npos) break;
			auto close = t.find('}', open);
			if (close == std::string::npos) break;
			auto nested = t.find('{', open + 1);
			if (nested != std::string::npos && nested < close) { pos = open + 1; continue; }
			std::string chunk = t.substr(open, close - open + 1);
			pos = close + 1;
			double fr = 0, btn = 1, p2 = 0, down = 1;
			if (!getField(chunk, "frame", fr)) continue;
			getField(chunk, "btn", btn);
			if (!getField(chunk, "2p", p2)) {
				if (!getField(chunk, "player2", p2)) getField(chunk, "p2", p2);
			}
			getField(chunk, "down", down);
			m.push_back({(int)fr, (int)btn, down != 0, p2 == 0});
		}
		std::stable_sort(m.begin(), m.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
		return m;
	}

	inline std::vector<Input> parseAny(std::string const& t, bool& unsupported) {
		unsupported = false;
		for (unsigned char c : t) {
			if (c == 0) { unsupported = true; return {}; } // binary file
		}
		auto i = t.find_first_not_of(" \t\r\n");
		if (i == std::string::npos) return {};
		std::vector<Input> m;
		if (t[i] == '{' || t[i] == '[') m = parseJson(t);
		else m = parseText(t);
		if (m.empty()) unsupported = true;
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
		auto m = parseText(res.unwrap());
		if (m.empty()) return false;
		macro = m;
		return true;
	}

	inline void autosave() {
		if (macro.empty()) return;
		saveAs("last", macro);
	}

	inline void importText(std::string const& text) {
		bool unsupported = false;
		auto m = parseAny(text, unsupported);
		if (m.empty()) {
			notify(unsupported ? "Format not supported (binary or unknown)" : "No inputs found", NotificationIcon::Error);
			return;
		}
		macro = m;
		auto name = newName("import");
		saveAs(name, m);
		notify(fmt::format("Imported {} ({} inputs)", name, m.size()), NotificationIcon::Success);
	}

	inline void importFromFile() {
		geode::async::spawn(
			utils::file::pick(utils::file::PickMode::OpenFile, utils::file::FilePickOptions()),
			[](auto res) {
				if (res.isErr()) {
					notify("Can't open file picker", NotificationIcon::Error);
					return;
				}
				auto opt = res.unwrap();
				if (!opt) {
					notify("No file picked", NotificationIcon::Error);
					return;
				}
				auto text = utils::file::readString(*opt);
				if (text.isErr()) {
					notify("Can't read file", NotificationIcon::Error);
					return;
				}
				importText(text.unwrap());
			}
		);
	}
}

class $modify(RBGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		bool inPlay = this == static_cast<GJBaseGameLayer*>(PlayLayer::get());

		if (inPlay) {
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
		RB::deathSaved = false;
		if (RB::mode == Mode::Recording) {
			if (!RB::macro.empty()) RB::prev = RB::macro;
			RB::macro.clear();
		}
	}

	// on death: save everything recorded in this attempt
	void destroyPlayer(PlayerObject* player, GameObject* object) {
		PlayLayer::destroyPlayer(player, object);
		if (RB::mode == Mode::Recording && !RB::deathSaved && !RB::macro.empty()) {
			RB::deathSaved = true;
			RB::autosave();
		}
	}

	void levelComplete() {
		if (RB::mode == Mode::Recording) {
			RB::autosave();
			RB::notify("Macro finished - saved as last", NotificationIcon::Success);
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

static ButtonSprite* rbSprite(char const* text, int width = 100, float scale = 0.8f) {
	return ButtonSprite::create(text, width, true, "bigFont.fnt", "GJ_button_01.png", 30.f, scale);
}

static CCPoint rbCenter(FLAlertLayer* a) {
	auto win = CCDirector::get()->getWinSize();
	return a->m_buttonMenu->convertToNodeSpace({win.width / 2, win.height / 2});
}

// ---------- list of saved macros (scrollable) ----------
class RBList : public CCNode {
public:
	FLAlertLayer* m_alert = nullptr;

	static void open() {
		RB::loadIndex();
		int total = (int)RB::names.size();

		auto title = fmt::format("Saved macros ({})", total);
		auto alert = FLAlertLayer::create(nullptr, title.c_str(), " ", "Close", nullptr, 340.f, false, 300.f, 1.f);

		auto h = new RBList();
		h->init();
		h->autorelease();
		h->m_alert = alert;
		alert->m_mainLayer->addChild(h);

		auto c = rbCenter(alert);

		if (total == 0) {
			auto lbl = CCLabelBMFont::create("No saved macros", "bigFont.fnt");
			lbl->setScale(0.6f);
			lbl->setPosition({c.x, c.y + 10.f});
			alert->m_buttonMenu->addChild(lbl);
		} else {
			constexpr float W = 270.f;
			constexpr float H = 170.f;
			constexpr float ROW = 42.f;
			float contentH = std::max(H, total * ROW);

			auto scroll = ScrollLayer::create({W, H});
			scroll->setPosition({c.x - W / 2, c.y - H / 2 + 5.f});
			scroll->m_contentLayer->setContentSize({W, contentH});

			auto menu = CCMenu::create();
			menu->setPosition({0.f, 0.f});
			menu->setContentSize({W, contentH});
			for (int i = 0; i < total; i++) {
				std::string label = RB::names[i];
				if (label.size() > 16) label = label.substr(0, 16);
				if (i == RB::selected) label = "> " + label;
				auto btn = CCMenuItemSpriteExtra::create(rbSprite(label.c_str(), 260), h, menu_selector(RBList::onPick));
				btn->setTag(i);
				btn->setPosition({W / 2, contentH - (i + 0.5f) * ROW});
				menu->addChild(btn);
			}
			scroll->m_contentLayer->addChild(menu);
			scroll->moveToTop();
			alert->m_buttonMenu->getParent()->addChild(scroll, 5);
		}
		handleTouchPriority(alert);
		alert->show();
	}

	void onPick(CCObject* s) {
		int i = static_cast<CCNode*>(s)->getTag();
		auto alert = m_alert;
		this->retain();
		alert->keyBackClicked();
		if (i >= 0 && i < (int)RB::names.size()) {
			auto name = RB::names[i];
			RB::selected = i;
			if (RB::loadByName(name)) {
				RB::notify(fmt::format("Selected {} ({} inputs)", name, RB::macro.size()), NotificationIcon::Success);
			} else {
				RB::notify(fmt::format("Can't load {}", name), NotificationIcon::Error);
			}
		}
		this->release();
	}
};

// ---------- main hub menu ----------
class RBHub : public CCNode {
public:
	FLAlertLayer* m_alert = nullptr;
	PauseLayer* m_pause = nullptr;

	static void open(PauseLayer* pause) {
		auto alert = FLAlertLayer::create(nullptr, "ReplayBot", " ", "Close", nullptr, 400.f, false, 300.f, 1.f);

		auto h = new RBHub();
		h->init();
		h->autorelease();
		h->m_alert = alert;
		h->m_pause = pause;
		alert->m_mainLayer->addChild(h);

		auto c = rbCenter(alert);
		auto add = [&](char const* text, SEL_MenuHandler sel, float x, float y) {
			auto btn = CCMenuItemSpriteExtra::create(rbSprite(text, 150), h, sel);
			btn->setPosition({c.x + x, c.y + y});
			alert->m_buttonMenu->addChild(btn);
		};
		add("Record", menu_selector(RBHub::onRec), -90.f, 70.f);
		add("Replay", menu_selector(RBHub::onPlay), 90.f, 70.f);
		add("Stop", menu_selector(RBHub::onStop), -90.f, 25.f);
		add("Save", menu_selector(RBHub::onSave), 90.f, 25.f);
		add("Select", menu_selector(RBHub::onSelect), -90.f, -20.f);
		add("Import", menu_selector(RBHub::onImport), 90.f, -20.f);
		add("Paste", menu_selector(RBHub::onPaste), 0.f, -65.f);
		handleTouchPriority(alert);
		alert->show();
	}

	void onRec(CCObject*) {
		auto pause = m_pause;
		this->retain();
		m_alert->keyBackClicked();
		RB::macro.clear();
		RB::prev.clear();
		RB::mode = Mode::Recording;
		RB::notify("Recording...");
		pause->onRestart(nullptr);
		this->release();
	}

	void onPlay(CCObject*) {
		auto pause = m_pause;
		this->retain();
		m_alert->keyBackClicked();
		if (RB::macro.empty()) {
			RB::loadIndex();
			bool ok = false;
			if (RB::selected >= 0 && RB::selected < (int)RB::names.size()) ok = RB::loadByName(RB::names[RB::selected]);
			if (!ok) ok = RB::loadByName("last");
			if (!ok) {
				RB::notify("No macro found", NotificationIcon::Error);
				this->release();
				return;
			}
		}
		RB::mode = Mode::Replaying;
		RB::notify(fmt::format("Replaying {} inputs", RB::macro.size()));
		pause->onRestart(nullptr);
		this->release();
	}

	void onStop(CCObject*) {
		this->retain();
		m_alert->keyBackClicked();
		if (RB::mode == Mode::Recording) {
			if (RB::macro.empty()) RB::macro = RB::prev;
			if (!RB::macro.empty()) {
				RB::autosave();
				RB::notify(fmt::format("Stopped: {} inputs (press Save)", RB::macro.size()), NotificationIcon::Success);
			} else {
				RB::notify("0 inputs recorded", NotificationIcon::Error);
			}
		} else if (RB::mode == Mode::Replaying) {
			RB::notify(fmt::format("Injected {}/{}", RB::injected, RB::macro.size()));
		}
		RB::mode = Mode::Idle;
		this->release();
	}

	void onSave(CCObject*) {
		this->retain();
		m_alert->keyBackClicked();
		if (RB::mode == Mode::Recording && RB::macro.empty()) RB::macro = RB::prev;
		if (RB::macro.empty()) {
			RB::notify("Nothing to save", NotificationIcon::Error);
		} else {
			auto name = RB::newName("macro");
			RB::saveAs(name, RB::macro);
			RB::notify(fmt::format("Saved as {} ({} inputs)", name, RB::macro.size()), NotificationIcon::Success);
		}
		this->release();
	}

	void onSelect(CCObject*) {
		this->retain();
		m_alert->keyBackClicked();
		RBList::open();
		this->release();
	}

	void onImport(CCObject*) {
		this->retain();
		m_alert->keyBackClicked();
		RB::importFromFile();
		this->release();
	}

	void onPaste(CCObject*) {
		this->retain();
		m_alert->keyBackClicked();
		RB::importText(utils::clipboard::read());
		this->release();
	}
};

class $modify(RBPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		auto win = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		menu->setPosition({win.width - 60.f, win.height / 2});
		auto btn = CCMenuItemSpriteExtra::create(rbSprite("ReplayBot", 110), this, menu_selector(RBPause::onHub));
		menu->addChild(btn);
		this->addChild(menu);
	}

	void onHub(CCObject*) {
		RBHub::open(static_cast<PauseLayer*>(this));
	}
};
