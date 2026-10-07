#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <chrono>
#include <array>
#include <vector>
#include <fstream>
#include <sstream>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

// ---- shared state ----
static Clock::time_point g_lastFrame = Clock::now();
static std::array<int, 9 + 1> g_buckets{};   // index 0..8 = 0-1 .. 8-10 ms, 9 = 10+
static int g_clicks = 0;
static double g_scoreSum = 0.0;
static float g_fps = 0.f;

struct Input { int step; int button; bool down; bool p2; };
static std::vector<Input> g_macro;
static size_t g_macroIdx = 0;
static bool g_botInjecting = false;

static bool g_hasLast = false;
static double g_hzSum = 0.0;
static int g_hzCount = 0;
static void resetStats() {
    g_buckets.fill(0); g_clicks = 0; g_scoreSum = 0.0; g_hasLast = false; g_hzSum = 0.0; g_hzCount = 0;
}

// Bucket by the Hz needed for the gap between two clicks (1000 / gap_ms)
static int bucketFor(double hz) {
    if (hz > 240) return 0;   // 0-1
    if (hz > 120) return 1;   // 1-2
    if (hz > 80)  return 2;   // 2-3
    if (hz > 60)  return 3;   // 3-4
    if (hz > 40)  return 4;   // 4-5
    if (hz > 30)  return 5;   // 5-6
    if (hz > 24)  return 6;   // 6-8
    if (hz > 12)  return 7;   // 8-10
    return 9;                 // 10+
}

static Clock::time_point g_lastClick;

static void recordClick() {
    auto now = Clock::now();
    // accuracy: how close to a frame boundary the click landed
    double off = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
    double frameMs = 1000.0 / Mod::get()->getSettingValue<double>("frame-hz");
    off = fmod(off, frameMs);
    g_scoreSum += 1.0 - off / frameMs;
    g_clicks++;
    if (g_hasLast) {
        double gap = std::chrono::duration<double, std::milli>(now - g_lastClick).count();
        if (gap > 0.01) {
            double hz = 1000.0 / gap;
            g_buckets[bucketFor(hz)]++;
            g_hzSum += std::min(hz, 240.0);
            g_hzCount++;
        }
    }
    g_lastClick = now;
    g_hasLast = true;
}

static std::filesystem::path macroPath() {
    return Mod::get()->getSaveDir() / "macro.txt";
}
static void saveMacro() {
    std::ofstream f(macroPath());
    for (auto& i : g_macro) f << i.step << ' ' << i.button << ' ' << i.down << ' ' << i.p2 << '\n';
}
static void loadMacro() {
    g_macro.clear();
    std::ifstream f(macroPath());
    Input i{}; int d, p;
    while (f >> i.step >> i.button >> d >> p) { i.down = d; i.p2 = p; g_macro.push_back(i); }
}

// ---- overlay ----
class $modify(HJPlayLayer, PlayLayer) {
    struct Fields {
        std::array<CCLabelBMFont*, 9 + 1> rows{};
        CCLabelBMFont* msLabel = nullptr;
        CCLabelBMFont* hzLabel = nullptr;
        CCLabelBMFont* accLabel = nullptr;
        CCLabelBMFont* fpsLabel = nullptr;
        CCNode* root = nullptr;
    };

    bool init(GJGameLevel* lvl, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(lvl, useReplay, dontCreateObjects)) return false;
        resetStats();
        g_macroIdx = 0;
        if (Mod::get()->getSettingValue<bool>("bot-record")) g_macro.clear();
        if (Mod::get()->getSettingValue<bool>("bot-play")) loadMacro();

        auto f = m_fields.self();
        f->root = CCNode::create();
        f->root->setZOrder(1000);
        auto win = CCDirector::get()->getWinSize();

        // top -> bottom: 10+, 8-10, 6-8, 5-6, 4-5, 3-4, 2-3, 1-2, 0-1
        const char* names[10] = {"10+:", "8-10:", "6-8:", "5-6:", "4-5:", "3-4:", "2-3:", "1-2:", "0-1:", ""};
        ccColor3B cols[9] = {
            {190,130,255}, {130,140,255}, {120,190,255}, {110,235,235}, {120,230,120},
            {255,240,110}, {255,170,90}, {255,110,110}, {255,100,140}};
        float y = win.height - 18.f;
        for (int r = 0; r < 9; r++) {
            auto name = CCLabelBMFont::create(names[r], "bigFont.fnt");
            name->setScale(0.35f); name->setColor(cols[r]);
            name->setAnchorPoint({0, 0.5f}); name->setPosition({6.f, y});
            f->root->addChild(name);
            auto val = CCLabelBMFont::create("0", "bigFont.fnt");
            val->setScale(0.35f); val->setColor(cols[r]);
            val->setAnchorPoint({0, 0.5f}); val->setPosition({62.f, y});
            f->root->addChild(val);
            f->rows[r] = val;
            y -= 14.f;
        }
        f->msLabel = CCLabelBMFont::create("", "bigFont.fnt");
        f->msLabel->setScale(0.35f); f->msLabel->setColor({190,190,190});
        f->msLabel->setAnchorPoint({0, 0.5f}); f->msLabel->setPosition({6.f, y - 4.f});
        f->root->addChild(f->msLabel);
        y -= 14.f;
        f->hzLabel = CCLabelBMFont::create("", "bigFont.fnt");
        f->hzLabel->setScale(0.35f); f->hzLabel->setColor({190,190,190});
        f->hzLabel->setAnchorPoint({0, 0.5f}); f->hzLabel->setPosition({6.f, y - 4.f});
        f->root->addChild(f->hzLabel);

        f->accLabel = CCLabelBMFont::create("100.00%", "bigFont.fnt");
        f->accLabel->setScale(0.4f); f->accLabel->setAnchorPoint({0, 0.5f});
        f->accLabel->setPosition({6.f, 40.f});
        f->root->addChild(f->accLabel);
        f->fpsLabel = CCLabelBMFont::create("0.00", "bigFont.fnt");
        f->fpsLabel->setScale(0.4f); f->fpsLabel->setAnchorPoint({0, 0.5f});
        f->fpsLabel->setPosition({6.f, 22.f});
        f->root->addChild(f->fpsLabel);

        f->root->setVisible(Mod::get()->getSettingValue<bool>("show-overlay"));
        this->addChild(f->root);
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        g_lastFrame = Clock::now();
        g_fps = dt > 0 ? 1.f / dt : 0.f;
        auto f = m_fields.self();
        if (!f->root) return;
        for (int r = 0; r < 9; r++) {
            int idx = r == 0 ? 9 : 9 - r - 1; // row0=10+ ... row8=0-1
            f->rows[r]->setString(std::to_string(g_buckets[idx]).c_str());
        }
        double hz = Mod::get()->getSettingValue<double>("frame-hz");
        f->msLabel->setString(fmt::format("{:.2f}MS", 1000.0 / hz).c_str());
        f->hzLabel->setString(fmt::format("{:.0f}Hz", hz).c_str());
        double acc = g_clicks ? 100.0 * g_scoreSum / g_clicks : 100.0;
        f->accLabel->setString(fmt::format("{:.2f}%", acc).c_str());
        double prec = g_hzCount ? g_hzSum / g_hzCount : 0.0;
        f->fpsLabel->setString(fmt::format("{:.2f}", prec).c_str());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        resetStats();
        g_macroIdx = 0;
        if (Mod::get()->getSettingValue<bool>("bot-record")) g_macro.clear();
    }

    void onQuit() {
        if (Mod::get()->getSettingValue<bool>("bot-record") && !g_macro.empty()) saveMacro();
        PlayLayer::onQuit();
    }
};

// ---- bot: record + replay on physics steps ----
class $modify(HJBaseLayer, GJBaseGameLayer) {
    int currentStep() { return m_gameState.m_currentProgress; }

    void processCommands(float dt) {
        if (Mod::get()->getSettingValue<bool>("bot-play") && !g_botInjecting) {
            g_botInjecting = true;
            while (g_macroIdx < g_macro.size() && g_macro[g_macroIdx].step <= currentStep()) {
                auto& i = g_macro[g_macroIdx++];
                if (i.down) GJBaseGameLayer::pushButton(i.button, i.p2);
                else GJBaseGameLayer::releaseButton(i.button, i.p2);
            }
            g_botInjecting = false;
        }
        GJBaseGameLayer::processCommands(dt);
    }

    bool pushButton(int button, bool p2) {
        if (!g_botInjecting) {
            recordClick();
            if (Mod::get()->getSettingValue<bool>("bot-record"))
                g_macro.push_back({currentStep(), button, true, p2});
        }
        return GJBaseGameLayer::pushButton(button, p2);
    }

    bool releaseButton(int button, bool p2) {
        if (!g_botInjecting && Mod::get()->getSettingValue<bool>("bot-record"))
            g_macro.push_back({currentStep(), button, false, p2});
        return GJBaseGameLayer::releaseButton(button, p2);
    }
};
