#ifndef RENDERHELPER_H
#define RENDERHELPER_H

#include <vector>

// net.minecraft.src.RenderHelper
class RenderHelper {
public:
    RenderHelper();

    static void disableStandardItemLighting();
    static void enableStandardItemLighting();
    static void enableGUIStandardItemLighting();

private:
    static std::vector<float> field_1695_a;
};

#endif