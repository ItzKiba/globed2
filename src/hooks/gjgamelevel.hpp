#pragma once
#include <Geode/modify/GJGameLevel.hpp>

// revolutionary
class $modify(HookedGJGameLevel, GJGameLevel) {
    bool shouldTransitionWithPopScene = false;
};
