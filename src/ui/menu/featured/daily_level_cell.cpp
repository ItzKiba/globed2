#include "daily_level_cell.hpp"

#include <managers/daily_manager.hpp>

using namespace geode::prelude;

class NewLevelCell : public LevelCell {
public:
    void draw() override {
        // balls
    }

    NewLevelCell(char const* p0, float p1, float p2) : LevelCell(p0, p1, p2) {};
};

bool GlobedDailyLevelCell::init() {
    if (!CCLayer::init()) return false;

    auto winSize = CCDirector::sharedDirector()->getWinSize();

    darkBackground = Build<CCScale9Sprite>::create("square02_001.png")
        .contentSize({CELL_WIDTH, CELL_HEIGHT})
        .opacity(75)
        .zOrder(2)
        .parent(this);

    loadingCircle = Build<LoadingCircle>::create()
        .zOrder(-5)
        .pos(winSize * -0.5)
        .opacity(100)
        .parent(this);

    // dont replace this with ->show() and look in the bottom left!!! (worst mistake of my life)
    loadingCircle->runAction(CCRepeatForever::create(CCSequence::create(CCRotateBy::create(1.f, 360.f), nullptr)));

    DailyManager::get().getStoredLevel([this](GJGameLevel* level, const GlobedFeaturedLevel& meta) {
        this->level = level;
        this->rating = meta.rateTier;
        this->editionNum = meta.id;
        this->createCell(level);
    });

    return true;
}

GlobedDailyLevelCell::~GlobedDailyLevelCell() {
    DailyManager::get().clearWebCallback();
}

void GlobedDailyLevelCell::createCell(GJGameLevel* level) {
    loadingCircle->fadeAndRemove();

    Build<CCScale9Sprite>::create("GJ_square02.png")
        .contentSize({CELL_WIDTH, CELL_HEIGHT})
        .zOrder(5)
        .pos(darkBackground->getScaledContentSize() / 2)
        .parent(darkBackground)
        .store(background);

    Build<CCMenu>::create()
        .zOrder(6)
        .pos({CELL_WIDTH - 75, CELL_HEIGHT / 2})
        .parent(background)
        .store(menu);

    auto crown = Build<CCSprite>::createSpriteName("icon-crown.png"_spr)
        .pos({background->getScaledContentWidth() / 2, CELL_HEIGHT + 11.f})
        .zOrder(6)
        .parent(background);

    int frameValue = static_cast<int>(level->m_difficulty);

    auto levelcell = new NewLevelCell("baller", CELL_WIDTH - 15, CELL_HEIGHT - 25);
    levelcell->autorelease();
    levelcell->loadFromLevel(level);
    levelcell->setPosition({7.5f, 12.5f});
    background->addChild(levelcell);

    auto cvoltonID = levelcell->m_mainLayer->getChildByIDRecursive("cvolton.betterinfo/level-id-label");
    if (cvoltonID != nullptr) {
        cvoltonID->setVisible(false);
    }

    auto playBtn = typeinfo_cast<CCMenuItemSpriteExtra*>(levelcell->m_mainLayer->getChildByIDRecursive("view-button"));
    if (playBtn != nullptr) {
        playBtn->setSprite(CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png"));
        playBtn->getNormalImage()->setScale(0.75);
        playBtn->setContentSize(playBtn->getNormalImage()->getScaledContentSize());
        playBtn->getNormalImage()->setPosition(playBtn->getNormalImage()->getScaledContentSize() / 2);
    }

    auto diffContainer = levelcell->m_mainLayer->getChildByIDRecursive("difficulty-container");
    if (diffContainer != nullptr) {
        diffContainer->setPositionX(diffContainer->getPositionX() - 2.f);
    }

    CCNode* editionNode = Build<CCNode>::create()
        .pos({0, CELL_HEIGHT + 10.f})
        .scale(0.6f)
        .parent(background);

    CCSprite* editionBadge = Build<CCSprite>::createSpriteName("icon-edition.png"_spr)
        .pos({16.f, -0.5f})
        .scale(0.45f)
        .parent(editionNode);

    CCLabelBMFont* editionLabel = Build<CCLabelBMFont>::create(fmt::format("#{}", editionNum).c_str(), "bigFont.fnt")
        .scale(0.60f)
        .color({255, 181, 102})
        .anchorPoint({0, 0.5})
        .pos({10.f + editionBadge->getScaledContentWidth(), 0})
        .parent(editionNode);
    editionLabel->runAction(CCRepeatForever::create(CCSequence::create(CCTintTo::create(0.75, 255, 243, 143), CCTintTo::create(0.75, 255, 181, 102), nullptr)));

    CCScale9Sprite* editionBG = Build<CCScale9Sprite>::create("square02_small.png")
        .opacity(75)
        .zOrder(-1)
        .anchorPoint({0, 0.5})
        .contentSize({editionBadge->getScaledContentWidth() + editionLabel->getScaledContentWidth() + 16.f, 30.f})
        .parent(editionNode);

    GJDifficultySprite* diff = typeinfo_cast<GJDifficultySprite*>(levelcell->m_mainLayer->getChildByIDRecursive("difficulty-sprite"));
    DailyManager::get().attachRatingSprite(rating, diff);
}

GlobedDailyLevelCell* GlobedDailyLevelCell::create() {
    auto ret = new GlobedDailyLevelCell;
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }

    delete ret;
    return nullptr;
}