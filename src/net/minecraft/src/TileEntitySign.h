#pragma once

#include "TileEntity.h"
#include "java/String.h"

class TileEntitySign : public TileEntity {
public:
    TileEntitySign();

    void writeToNBT(NBTTagCompound* nbttagcompound) override;
    void readFromNBT(NBTTagCompound* nbttagcompound) override;

    bool isEditable() const;
    void setEditable(bool editable);

    jstring signText[4];
    int lineBeingEdited;

    // TileEntitySignRenderer's own cache: renderTileEntitySignParts() used to
    // call FontRenderer::getStringWidth() (a heap-allocating UTF-16
    // conversion) for all 4 lines, every frame this sign is visible, even
    // though signText only actually changes while a player is editing it
    // (GuiEditSign) -- not every render frame for the overwhelming majority
    // of a sign's lifetime. Lives here (rather than keyed by pointer in the
    // renderer, which is a single object shared by every sign) since it's
    // exactly the per-sign state it depends on. Invalidated whenever the
    // rendered text for a line (signText[k], plus the "> ... <" edit-cursor
    // decoration when k == lineBeingEdited) no longer matches what's cached.
    std::string cachedRenderedLine[4];
    int_t cachedRenderedWidth[4] = {0, 0, 0, 0};
    bool cachedLineValid[4] = {false, false, false, false};

private:
    bool editable;
};
