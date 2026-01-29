#include <emscripten/bind.h>
#include <emscripten/val.h>
#include "pixel_preview.h"
#include "pixel_core.h"

using namespace emscripten;

// Helper to get frame data as a JavaScript typed array view
val getFrameDataView(const PixelPreview& preview) {
    const uint8_t* data = preview.getFrameData();
    size_t size = preview.getFrameSize();
    return val(typed_memory_view(size, data));
}

// Helper to get effect list as JavaScript array
val getEffectListJS() {
    auto effects = PixelPreview::getEffectList();
    val arr = val::array();
    for (size_t i = 0; i < effects.size(); ++i) {
        arr.set(i, effects[i]);
    }
    return arr;
}

EMSCRIPTEN_BINDINGS(pixel_preview) {
    class_<PixelPreview>("PixelPreview")
        .constructor<uint16_t, bool, uint32_t>()
        .constructor<uint16_t, bool>()
        .function("setEffect", &PixelPreview::setEffect)
        .function("setColor", &PixelPreview::setColor)
        .function("setBrightness", &PixelPreview::setBrightness)
        .function("setSpeed", &PixelPreview::setSpeed)
        .function("tick", &PixelPreview::tick)
        .function("reset", &PixelPreview::reset)
        .function("setRandomSeed", &PixelPreview::setRandomSeed)
        .function("getFrameData", &getFrameDataView)
        .function("getFrameSize", &PixelPreview::getFrameSize)
        .function("getLedCount", &PixelPreview::getLedCount)
        .class_function("getEffectList", &getEffectListJS);

    // Version API
    function("getVersion", &getPixelDriverVersion);
    function("getVersionFull", &getPixelDriverVersionFull);
    function("getBuildTime", &getPixelDriverBuildTime);
}
