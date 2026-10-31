// SPDX-License-Identifier: GPL-3.0-only
#include "defaultpreferences.h"
#include <QSettings>

QVariantMap mangaPreferenceDefaults() {
    return {{"reading/spread",true}, {"reading/autoSpread",false},
        {"reading/rtl",true}, {"reading/cover",true}, {"reading/leadingBlank",true},
        {"reading/scrollTurnsPage",true}, {"source/recursive",false},
        {"General/watch",true}, {"navigation/count",20},
        {"cache/ahead",1}, {"cache/behind",0}, {"cache/fileMegabytes",0},
        {"slideshow/interval",100}, {"slideshow/scroll",true}, {"display/background",QString("#000000")},
        {"thumbnails/columns",0}, {"display/filename",false}, {"thumbnails/numbers",false}, {"folders/twoPane",true},
        {"thumbnails/width",64}, {"thumbnails/height",64}, {"thumbnails/horizontalGap",0},
        {"thumbnails/verticalGap",0}, {"thumbnails/filenameLines",1}, {"thumbnails/rightOrigin",false}, {"thumbnails/frames",false}, {"input/doubleClickMs",200},
        {"input/holdMs",1000}, {"input/gestureDistance",10},
        {"input/gestureSplitMs",100}, {"input/wheelPercent",9},
        {"input/wheelImageUnits",true}, {"input/smoothScroll",true},
        {"input/scrollSpeed",4000}, {"input/keyVerticalPercent",2000},
        {"processing/filterEnabled",false}, {"defaults/mangaPreset",1}};
}
void initializeMangaPreferences(QSettings &settings) {
    // Existing profiles retain both explicit values and their historical
    // fallbacks. Merely opening a saved profile never reapplies a preset.
    if (!settings.allKeys().isEmpty()) {
        if (!settings.contains("defaults/inputVersion")) {
            const QVariantMap old{{"left","next"},{"right","previous"},{"middle","loupe"},
                {"back","parentFolder"},{"forward","thumbnails"},{"wheelUp","previous"},
                {"wheelDown","next"},{"rightWheelUp","zoomIn"},{"rightWheelDown","zoomOut"},
                {"doubleleft","fullscreenGlobal"},{"doubleright",""},{"rightLeft",""},{"leftRight",""},
                {"rightMiddle",""},{"gesture/R","next"},{"gesture/L","previous"},
                {"gesture/U","parentFolder"},{"gesture/D","thumbnails"},{"gesture/LU",""},
                {"gesture/RD",""},{"gesture/DR",""},{"gesture/RR",""},{"gesture/LL",""},{"gesture/UD",""}};
            for(auto i=old.cbegin();i!=old.cend();++i) {
                const auto key="input/mouse/"+i.key();
                if(!settings.contains(key))settings.setValue(key,i.value());
            }
            settings.setValue("defaults/inputVersion",1);
        }
        return;
    }
    const auto values=mangaPreferenceDefaults();
    for (auto i=values.cbegin();i!=values.cend();++i) settings.setValue(i.key(),i.value());
    settings.setValue("defaults/inputVersion",1);
}
