#pragma once

#include <juce_core/juce_core.h>

namespace rysum
{
/** Room models that come with the plugin (the 3D view: "Raummodell laden"). They are made here as
 *  OBJ text - z up, metres, the inner corner of the room at 0 / 0 / 0 - and written into
 *  Documents/Rysum/Modelle, so they are ordinary files: loaded like any model, remembered by the
 *  project by their path, and open to be looked at or changed in a 3D program.
 *
 *  What makes a model work well here, and what these keep to: closed all round (floor, walls,
 *  roof), walls with a thickness, a free place where the listener stands, nothing lower than a
 *  step (45 cm) that is meant to stop you, steps of 20 cm where you are meant to climb, and a few
 *  hundred to two thousand triangles. */
struct BuiltInRoom
{
    const char *file;  // the file's name in the folder
    const char *name;  // as shown in the menu
    const char *about; // one line: what it is
    float listenX, listenY; // a good place to listen from (0 .. 1 across / along the model)
    juce::String (*make)();
};

/** all of them, in the order of the menu */
const std::vector<BuiltInRoom> &builtInRooms();

/** the colours the models use (the MTL file beside them) */
juce::String builtInRoomMaterials();
inline const char *builtInRoomMaterialFile()
{
    return "rysum_modelle.mtl";
}

/** Writes room 'index' (and the materials) into 'folder' if it is not there or differs from what
 *  this version makes. Returns the file (which may not exist if the folder cannot be written). */
juce::File writeBuiltInRoom(int index, const juce::File &folder);

/** the built-in room with this file name (-1 = none) */
int builtInRoomIndex(const juce::String &fileName);
} // namespace rysum
