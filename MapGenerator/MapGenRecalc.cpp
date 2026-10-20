// ============================================================================
// MapGenRecalc.cpp - the "RMG: Recalculating cell attributes" stage
// (sub_598960 @ 0x598e1f - 0x598e9e) and the data layer it needs.
//
// The stage is a walk: reset the CellIterator (sub_578350), call
// CellClass::RecalcAttributes (sub_47D2B0, decompile in 47D2B0.c) once per
// diamond cell with cellLevel = -1, then run the progress-bar / session side
// effects this port drops. The identical stage runs three more times later in
// the flow (0x598fb8 after tiberium, 0x59912a before the hills, 0x599354 after
// the LATs).
//
// RecalcAttributes reads exactly three fields off the overlay type sitting in
// cell->OverlayTypeIndex - LandType, Tiberium and NoUseTileLandType - so this
// file starts by building the port's OverlayTypeClass stand-in from the rules
// INI, mirroring OverlayTypeClass::LoadFromINI (0x5FE770).
// ============================================================================

#include "pch.h"
#include "MapGen.h"
#include <algorithm>
#include <initializer_list>
#include <utility>

// ---------------------------------------------------------------------------
// LandType - the 12 floor classes, in the exact order of the engine's string
// table off_81DA28 (read out of the image):
//   0 Clear, 1 Road, 2 Water, 3 Rock, 4 Wall, 5 Tiberium,
//   6 Beach, 7 Rough, 8 Ice, 9 Railroad, 10 Tunnel, 11 Weeds
// ---------------------------------------------------------------------------
static const char* const kLandTypeNames[12] =
{
    "Clear", "Road", "Water", "Rock", "Wall", "Tiberium",
    "Beach", "Rough", "Ice", "Railroad", "Tunnel", "Weeds",
};

// sub_48DF80 - string -> enum. Case-insensitive; both an unknown name and the
// literal "<none>" answer -1.
static int ParseLandType(const char* text)
{
    if (text == nullptr || _stricmp(text, "<none>") == 0)
        return -1;
    for (int i = 0; i < 12; ++i)
    {
        if (_stricmp(kLandTypeNames[i], text) == 0)
            return i;
    }
    return -1;
}

// (sub_48DFD0, the enum -> string half, has no user here: it only feeds
// sub_4754B0's ReadString default, and a freshly constructed entry's current
// value is always LandType_Clear - so the literal "Clear" default used below is
// equivalent to spelling sub_48DFD0(0).)

// CCINIClass::ReadBool (0x5295F0): the FIRST character of the value decides,
// case-insensitively - '0' / 'F' / 'N' answer false, '1' / 'T' / 'Y' answer
// true, and anything else (including an empty value) keeps the default.
static bool ParseIniBool(const char* text, bool defaultValue)
{
    if (text == nullptr || *text == '\0')
        return defaultValue;
    switch (toupper(static_cast<unsigned char>(*text)))
    {
    case '0': case 'F': case 'N': return false;
    case '1': case 'T': case 'Y': return true;
    default: return defaultValue;
    }
}

// ---------------------------------------------------------------------------
// LoadOverlayTypes - build the port's OverlayTypeClass::Array.
//
// Mirrors OverlayTypeClass::LoadFromINI (0x5FE770) for the three fields
// RecalcAttributes touches:
//     Land               -> landType           sub_4754B0: ReadString with the
//                                              entry's current value as the
//                                              default, parsed by sub_48DF80
//     Tiberium           -> tiberium           CCINIClass::ReadBool
//     NoUseTileLandType  -> noUseTileLandType  CCINIClass::ReadBool
//     then LoadFromINI's fix-up:
//         if (tiberium && landType == Clear) landType = Tiberium;
//
// The index -> name list is the rules INI's [OverlayTypes] section. The file
// (rulesmd.ini) lives in the project's rules-INI folder two levels above the
// built executable; a copy placed next to the executable is preferred so a
// hand-placed override wins. Nothing is fatal: a missing file leaves every
// entry absent, which the consumers read as LandType_Clear with both flags off.
//
// NOTE: sub_48DF80 answers -1 for a name it cannot parse, and LoadFromINI
// stores that -1 straight into the field, so an unparsable "Land=" really does
// leave landType at -1 rather than falling back to Clear.
// ---------------------------------------------------------------------------
void RandomMapGenerator::LoadOverlayTypes()
{
    overlayTypes_.clear();

    // ---- locate the rules INI ---------------------------------------------
    wchar_t wpath[MAX_PATH] = L"";
    if (!ResolveGameIni(L"rulesmd.ini", wpath, MAX_PATH))
        return;

    char path[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);

    // ---- every key of [OverlayTypes] --------------------------------------
    // GetPrivateProfileString with a null key name lists the section's keys,
    // each NUL-terminated, the list closed by an empty string.
    std::vector<char> keys(65536);
    const DWORD keyBytes = GetPrivateProfileStringA(
        "OverlayTypes", nullptr, "", keys.data(),
        static_cast<DWORD>(keys.size()), path);
    if (keyBytes == 0)
        return;                                    // no section / no file

    // ---- size the table to the highest index the file names ---------------
    int maxIndex = 0;
    for (const char* key = keys.data(); *key != '\0'; key += strlen(key) + 1)
    {
        const int index = atoi(key);
        if (index > maxIndex)
            maxIndex = index;
    }
    overlayTypes_.assign(static_cast<size_t>(maxIndex) + 1, OverlayTypeInfo());

    // ---- one entry per named index ----------------------------------------
    for (const char* key = keys.data(); *key != '\0'; key += strlen(key) + 1)
    {
        const int index = atoi(key);
        if (index <= 0)
            continue;                              // the shipped lists start at 1

        char name[64] = "";
        GetPrivateProfileStringA("OverlayTypes", key, "", name, sizeof(name), path);
        if (name[0] == '\0')
            continue;

        OverlayTypeInfo& info = overlayTypes_[static_cast<size_t>(index)];
        info.present = true;

        char text[64] = "";
        GetPrivateProfileStringA(name, "Land", "Clear", text, sizeof(text), path);
        info.landType = ParseLandType(text);       // may be -1, as in vanilla

        text[0] = '\0';
        GetPrivateProfileStringA(name, "Tiberium", "", text, sizeof(text), path);
        info.tiberium = ParseIniBool(text, false);

        text[0] = '\0';
        GetPrivateProfileStringA(name, "NoUseTileLandType", "", text,
                                 sizeof(text), path);
        info.noUseTileLandType = ParseIniBool(text, false);

        // The four flags sub_483C80 consults. "Crushable" belongs to the base
        // ObjectTypeClass::LoadFromINI (0x5F92D0), the rest to the overlay's own.
        text[0] = '\0';
        GetPrivateProfileStringA(name, "Crushable", "", text, sizeof(text), path);
        info.crushable = ParseIniBool(text, false);

        text[0] = '\0';
        GetPrivateProfileStringA(name, "Wall", "", text, sizeof(text), path);
        info.wall = ParseIniBool(text, false);

        text[0] = '\0';
        GetPrivateProfileStringA(name, "IsARock", "", text, sizeof(text), path);
        info.isARock = ParseIniBool(text, false);

        text[0] = '\0';
        GetPrivateProfileStringA(name, "IsRubble", "", text, sizeof(text), path);
        info.isRubble = ParseIniBool(text, false);

        // LoadFromINI's fix-up: ore floors that claim Clear really are ore.
        if (info.tiberium && info.landType == 0)   // LandType_Clear
            info.landType = 5;                     // LandType_Tiberium
    }

    // RulesClass::LoadFromINI reads this from [General] (0x66E7xx); the shipped
    // value is 2, which is the only setting that rewrites a floor to Rock.
    cliffBackImpassability_ = (int)GetPrivateProfileIntA(
        "General", "CliffBackImpassability", (UINT)-1, path);
}

// ---------------------------------------------------------------------------
// PassabilityIndex - sub_56D3F0 (0x56d3f0 - 0x56d426).
//
//   index = x + y * (W' + H' + 1)      vanilla: this[62] + this[61] + 1
//   if (index < 0)       return 0
//   if (index >= count)  return count - 1
//   return index
//
// The stride is the work array's own workSide and `count` is this[27], the
// element count of both arrays (workSide * workSide). A coordinate outside the
// diamond therefore clamps to one of the two ends instead of running off the
// buffer - which is what lets RecalcAttributes index the arrays from a raw
// (possibly out-of-diamond) MapCoords without a gate of its own.
// ---------------------------------------------------------------------------
int RandomMapGenerator::PassabilityIndex(int x, int y) const
{
    const int side = size_.workSide;                  // W' + H' + 1
    const int count = side * side;                    // MouseClass::Instance[27]

    const int index = x + y * side;
    if (index < 0)
        return 0;
    if (index >= count)
        return count - 1;
    return index;
}

// ---------------------------------------------------------------------------
// dword_8288E4 @ 0x8288E4 - the 16-entry TMP terrain type -> LandType table
// sub_544BE0 indexes with a tile cell's terrain byte (extra[+41]). Read out of
// the image: TMP terrain 15 (the cliff art) answers 3 = LandType_Rock.
// ---------------------------------------------------------------------------
static const int kLandTypeFromTerrainType[16] =
{
    0, 8, 8, 8, 8, 10, 9, 3, 3, 2, 6, 1, 1, 0, 7, 3
};

int RandomMapGenerator::LandTypeFromTerrainType(int terrainType)
{
    if (terrainType < 0 || terrainType >= 16)
        return 0;                                     // LandType_Clear
    return kLandTypeFromTerrainType[terrainType];
}

// ---------------------------------------------------------------------------
// TileCellAttrAt - the TMP record of tile `tile` at cell offset `cell` (the
// cell's Height). nullptr means "no image", which is the answer every vanilla
// helper gives for a tile it has no art for.
// ---------------------------------------------------------------------------
const TileCellAttr* RandomMapGenerator::TileCellAttrAt(int tile, int cell) const
{
    if (tile < 0 || tile >= static_cast<int>(tileCellAttrs_.size()))
        return nullptr;
    const std::vector<TileCellAttr>& cells =
        tileCellAttrs_[static_cast<size_t>(tile)];
    if (cell < 0 || cell >= static_cast<int>(cells.size()))
        return nullptr;
    return &cells[static_cast<size_t>(cell)];
}

// sub_544C20 - "does tile `tile` carry an image frame at cell `cell`?" Vanilla
// loads the image on demand (the a2 flag) and then tests
//     cell < CellsInX * CellsInY  &&  image[cell] != null
// A zero offset entry in the TMP is that same "no frame" answer here.
bool RandomMapGenerator::TileCellHasFrame(int tile, int cell) const
{
    const TileCellAttr* a = TileCellAttrAt(tile, cell);
    return a != nullptr && a->hasFrame;
}

// sub_544BE0 - the cell's LandType: its TMP terrain type through dword_8288E4.
// The helper answers 0 when the tile has no image.
int RandomMapGenerator::TileCellLandType(int tile, int cell) const
{
    const TileCellAttr* a = TileCellAttrAt(tile, cell);
    if (a == nullptr)
        return 0;
    return LandTypeFromTerrainType(a->terrainType);
}

// sub_5471B0 - the cell's ramp type (extra[+42]); 0 without an image.
int RandomMapGenerator::TileCellRampType(int tile, int cell) const
{
    const TileCellAttr* a = TileCellAttrAt(tile, cell);
    return (a == nullptr) ? 0 : static_cast<int>(a->rampType);
}

// sub_547150 - the CellsInX and the Y adjustment RecalcAttributes stores as the
// cell's ZAdjust:
//     *cellsInX = image[2]                                   (CellsInX)
//     y         = image[3]                                   (CellsInY)
//     if (image[cell] && (extra[+36] & 1))
//         y = CellsInY + extra[+4] - extra[+24]
// Answers false when the tile has no image (vanilla returns 0 there).
bool RandomMapGenerator::TileCellImageInfo(int tile, int cell, int* cellsInX,
                                           int* yAdjust) const
{
    const TileCellAttr* a = TileCellAttrAt(tile, cell);
    if (a == nullptr)
        return false;

    if (cellsInX != nullptr)
        *cellsInX = a->cellsInX;

    int y = a->cellsInY;
    if (a->hasFrame && (a->flags36 & 1) != 0)
        y = a->cellsInY + a->extra4 - a->extra24;

    if (yAdjust != nullptr)
        *yAdjust = y;
    return true;
}

// ---------------------------------------------------------------------------
// ParseTmpInto - read one TMP file into `out`, one entry per cell.
//
// Layout (see TileCellAttr):
//     +0   int32 CellsInX          +16  int32 offset[CellsInX * CellsInY]
//     +4   int32 CellsInY          extra[+36] dword flags
//     +8   int32 cellWidth         extra[+41] byte terrain type
//     +12  int32 cellHeight        extra[+42] byte ramp type
// Any failure leaves `out` empty, which the helpers read as "no image".
// ---------------------------------------------------------------------------
static void ParseTmpInto(std::vector<TileCellAttr>& out, const wchar_t* path)
{
    out.clear();

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart < 16 || size.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle(file);
        return;
    }

    std::vector<unsigned char> data(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(file, data.data(),
                             static_cast<DWORD>(data.size()), &got, nullptr) != 0;
    CloseHandle(file);
    if (!ok || got != data.size())
        return;

    RandomMapGenerator::ParseTmpBuffer(out, data.data(),
                                       static_cast<int>(data.size()));
}

// ---------------------------------------------------------------------------
// [FA2sp] ParseTmpBuffer - the same parser, but the bytes come from the caller
// instead of a file on disk. The editor extension extracts the TMP out of the
// game MIX (isotem.mix / isosno.mix) and hands the buffer over, so the header
// layout lives in exactly one place.
// ---------------------------------------------------------------------------
void RandomMapGenerator::ParseTmpBuffer(std::vector<TileCellAttr>& out,
                                        const unsigned char* base, int size)
{
    out.clear();
    if (base == nullptr || size < 16)
        return;

    int cellsInX = 0, cellsInY = 0;
    std::memcpy(&cellsInX, base + 0, 4);
    std::memcpy(&cellsInY, base + 4, 4);
    if (cellsInX <= 0 || cellsInY <= 0 || cellsInX > 64 || cellsInY > 64)
        return;

    const int count = cellsInX * cellsInY;
    if (16 + 4 * count > size)
        return;

    out.resize(static_cast<size_t>(count));
    for (int cell = 0; cell < count; ++cell)
    {
        TileCellAttr attr = TileCellAttr();
        attr.cellsInX = cellsInX;
        attr.cellsInY = cellsInY;

        int offset = 0;
        std::memcpy(&offset, base + 16 + 4 * cell, 4);
        if (offset > 0 && offset + 43 <= size)
        {
            attr.hasFrame    = true;
            attr.flags36     = base[offset + 36];
            attr.terrainType = base[offset + 41];
            attr.rampType    = base[offset + 42];
            std::memcpy(&attr.extra4,  base + offset + 4,  4);
            std::memcpy(&attr.extra24, base + offset + 24, 4);

            // The frame's two radar pixels (the RA2 image header, 52 bytes).
            // A header that stops before them keeps the zeroed defaults.
            if (offset + 49 <= size)
            {
                attr.radarRedLeft   = base[offset + 43];
                attr.radarGreenLeft = base[offset + 44];
                attr.radarBlueLeft  = base[offset + 45];
                attr.radarRedRight   = base[offset + 46];
                attr.radarGreenRight = base[offset + 47];
                attr.radarBlueRight  = base[offset + 48];
            }
        }
        out[static_cast<size_t>(cell)] = attr;
    }
}

// ---------------------------------------------------------------------------
// LoadTileCellAttrs - the port's stand-in for the tiles' TMP images.
//
// Vanilla keeps this inside IsometricTileTypeClass: every tile type points at a
// .tem / .sno file and GetImage() reads it on demand. The port has no tile
// objects, so it walks the theater INI's tile sets - the same [TileSetNNNN]
// walk LoadTheaterTiles performs - derives each tile's file name from the set's
// FileName plus the tile's position inside the set, and parses the TMP itself.
//
// The art sits under the project's tile-art folder, two levels above the
// executable: "温和" (temperate) for theater 0, "雪地" (snow) for theater 1.
// Tiles whose file is missing keep an empty entry, which reads as "no image".
// ---------------------------------------------------------------------------
void RandomMapGenerator::LoadTileCellAttrs(int theater)
{
    tileCellAttrs_.clear();

    wchar_t wdir[MAX_PATH];
    GetModuleFileNameW(nullptr, wdir, MAX_PATH);
    wchar_t* slash = wcsrchr(wdir, L'\\');
    if (slash)
        slash[1] = L'\0';

    // ---- locate the theater INI ------------------------------------------
    static const wchar_t* const kCandidates[] =
    {
        L"%s%s",                                       // next to the executable
        L"%s..\\%s",
        L"%s..\\..\\\u76f8\u5173INI\\%s",               // ...\<rules folder>
    };
    const wchar_t* const iniName =
        (theater == 1) ? L"snowmd.ini" : L"temperatmd.ini";

    wchar_t wini[MAX_PATH] = L"";
    bool found = false;
    for (int i = 0; i < 3 && !found; ++i)
    {
        swprintf_s(wini, kCandidates[i], wdir, iniName);
        if (GetFileAttributesW(wini) != INVALID_FILE_ATTRIBUTES)
            found = true;
    }
    if (!found)
        return;

    char iniPath[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wini, -1, iniPath, MAX_PATH, nullptr, nullptr);

    // ---- [TileSetNNNN] sections in numeric order -------------------------
    std::vector<std::pair<int, int> > sets;            // (section, TilesInSet)
    {
        std::vector<char> names(64 * 1024, '\0');
        if (GetPrivateProfileSectionNamesA(names.data(),
                                           static_cast<DWORD>(names.size()),
                                           iniPath) > 0)
        {
            for (const char* s = names.data(); *s; s += std::strlen(s) + 1)
            {
                if (_strnicmp(s, "TileSet", 7) != 0)
                    continue;
                const int tiles =
                    (int)GetPrivateProfileIntA(s, "TilesInSet", 0, iniPath);
                sets.push_back(std::make_pair(std::atoi(s + 7), tiles));
            }
        }
    }
    std::sort(sets.begin(), sets.end());

    // ---- the theater's art folder ----------------------------------------
    wchar_t artDir[MAX_PATH];
    swprintf_s(artDir, L"%s..\\..\\Tile\u8d44\u6e90\\%s\\", wdir,
               (theater == 1) ? L"\u96ea\u5730" : L"\u6e29\u548c");
    const wchar_t* const ext = (theater == 1) ? L"sno" : L"tem";

    // ---- one TMP per tile -------------------------------------------------
    int running = 0;
    for (size_t i = 0; i < sets.size(); ++i)
    {
        const int tilesInSet = sets[i].second;
        if (tilesInSet <= 0)
            continue;

        if (static_cast<int>(tileCellAttrs_.size()) < running + tilesInSet)
            tileCellAttrs_.resize(static_cast<size_t>(running + tilesInSet));

        char section[32];
        sprintf_s(section, "TileSet%04d", sets[i].first);
        char fileName[64] = "";
        GetPrivateProfileStringA(section, "FileName", "", fileName,
                                 sizeof(fileName), iniPath);

        if (fileName[0] != '\0')
        {
            wchar_t wname[64];
            MultiByteToWideChar(CP_ACP, 0, fileName, -1, wname, 64);

            for (int k = 0; k < tilesInSet; ++k)
            {
                wchar_t wfile[MAX_PATH];
                swprintf_s(wfile, L"%s%s%02d.%s", artDir, wname, k + 1, ext);
                ParseTmpInto(tileCellAttrs_[static_cast<size_t>(running + k)],
                             wfile);
            }
        }
        running += tilesInSet;
    }
}

// ---------------------------------------------------------------------------
// ParseIniPercent - the rules INI's "100%" / "50%" / "0%" spelling.
// ---------------------------------------------------------------------------
static float ParseIniPercent(const char* text)
{
    if (text == nullptr)
        return 0.0f;
    return static_cast<float>(atof(text) / 100.0);
}

// ---------------------------------------------------------------------------
// LoadGroundTypes - build GroundType::Array (0x89EA40, 12 entries).
//
// Every floor section is named after its LandType and carries one percentage
// per speed type, plus Buildable:
//     Foot  Track  Wheel  Float  Hover  Amphibious  FloatBeach
// The engine keeps them in Cost[0..6] of an 8-slot array in that same order, so
// Cost[2] is the WHEEL column - the one sub_483C80 tests.
// ---------------------------------------------------------------------------
void RandomMapGenerator::LoadGroundTypes()
{
    static const char* const kSpeedKeys[7] =
    {
        "Foot", "Track", "Wheel", "Float", "Hover", "Amphibious", "FloatBeach"
    };

    groundTypes_.assign(12, GroundTypeInfo());

    // The same rules INI the overlay table comes from.
    wchar_t wpath[MAX_PATH] = L"";
    if (!ResolveGameIni(L"rulesmd.ini", wpath, MAX_PATH))
        return;

    char path[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);

    for (int landType = 0; landType < 12; ++landType)
    {
        const char* section = kLandTypeNames[landType];
        GroundTypeInfo& gt = groundTypes_[static_cast<size_t>(landType)];

        for (int s = 0; s < 7; ++s)
        {
            char text[64] = "";
            GetPrivateProfileStringA(section, kSpeedKeys[s], "0", text,
                                     sizeof(text), path);
            gt.cost[s] = ParseIniPercent(text);
        }

        char text[64] = "";
        GetPrivateProfileStringA(section, "Buildable", "", text, sizeof(text), path);
        gt.buildable = ParseIniBool(text, false);
    }
}

const GroundTypeInfo& RandomMapGenerator::GroundTypeFor(int landType) const
{
    static const GroundTypeInfo kAbsent;
    if (landType < 0 || landType >= static_cast<int>(groundTypes_.size()))
        return kAbsent;
    return groundTypes_[static_cast<size_t>(landType)];
}

// ---------------------------------------------------------------------------
// LoadTiberiums - build TiberiumClass::Array.
//
// [Tiberiums] names the ore types in order; each one's own section carries an
// "Image=" NUMBER that LoadFromINI (0x721A50) maps to a hard-coded overlay:
//     Image = 2 -> OverlayTypeClass::Array[27]     (the gem floor)
//     Image = 3 -> Array[127]
//     Image = 4 -> Array[147]
//     anything else, 1 included -> Array[102]
// plus NumImages = 12 and the +0xEC field = 8 (0x721cc6 sets it literally). A
// section without an "Image" key leaves the engine's Image untouched, which is
// not a usable overlay index, so such an entry is skipped here.
// ---------------------------------------------------------------------------
void RandomMapGenerator::LoadTiberiums()
{
    tiberiums_.clear();

    static const int kImageToOverlayStart[5] =
    {
        102,    // Image = 0
        102,    // Image = 1 (Riparius)
        27,     // Image = 2 (Cruentus - the gems)
        127,    // Image = 3 (Vinifera)
        147,    // Image = 4 (Aboreus)
    };

    // The same rules INI the other tables came from.
    wchar_t wpath[MAX_PATH] = L"";
    if (!ResolveGameIni(L"rulesmd.ini", wpath, MAX_PATH))
        return;

    char path[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);

    std::vector<char> keys(4096, '\0');
    if (GetPrivateProfileStringA("Tiberiums", nullptr, "", keys.data(),
                                 static_cast<DWORD>(keys.size()), path) == 0)
        return;

    for (const char* key = keys.data(); *key != '\0'; key += std::strlen(key) + 1)
    {
        char name[64] = "";
        GetPrivateProfileStringA("Tiberiums", key, "", name, sizeof(name), path);
        if (name[0] == '\0')
            continue;

        const int image = (int)GetPrivateProfileIntA(name, "Image", -1, path);
        if (image < 0)
            continue;                              // no Image key: unusable

        TiberiumInfo t;
        t.overlayStart = (image <= 4) ? kImageToOverlayStart[image] : 102;
        t.numImages = 12;
        t.extra = 8;
        t.arrayIndex = std::atoi(key);
        tiberiums_.push_back(t);
    }
}

// ---------------------------------------------------------------------------
// ToTiberiumIdx - 0x5FDD20 (size 0xBE).
//
//   if (overlayIndex == -1) return -1;
//   if (!OverlayTypeClass::Array[overlayIndex]->Tiberium) return -1;
//   for (i = 0; i < TiberiumClass::Array.Count; ++i)
//   {
//       start = Array[i]->Image->ArrayIndex;
//       if (overlayIndex in [start, start + NumImages))                      return Array[i]->ArrayIndex;
//       if (overlayIndex in [start + NumImages, start + NumImages + +0xEC))  return Array[i]->ArrayIndex;
//   }
//   return 0;                                    // ore, but no entry matched
//
// The caller only compares the answer against -1, so the "0" outcome and a real
// ore index are equivalent to it.
// ---------------------------------------------------------------------------
int RandomMapGenerator::ToTiberiumIdx(int overlayIndex) const
{
    if (overlayIndex == -1)
        return -1;
    if (overlayIndex < 0
        || overlayIndex >= static_cast<int>(overlayTypes_.size()))
        return -1;
    if (!overlayTypes_[static_cast<size_t>(overlayIndex)].tiberium)
        return -1;

    for (size_t i = 0; i < tiberiums_.size(); ++i)
    {
        const TiberiumInfo& t = tiberiums_[i];
        const int firstEnd = t.overlayStart + t.numImages;
        if (overlayIndex >= t.overlayStart && overlayIndex < firstEnd)
            return t.arrayIndex;
        if (overlayIndex >= firstEnd && overlayIndex < firstEnd + t.extra)
            return t.arrayIndex;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// RecalcCellPassability - sub_483C80 (0x483c80 - 0x483e29).
//
// Vanilla body, in order:
//     if (!IsWithinUsableArea(coords, 1))  { Passability = OutsideMap;  return; }
//     ov = cell->OverlayTypeIndex;
//     if (ov != -1)
//     {
//         t = OverlayTypeClass::Array[ov];
//         if (t->Crushable)                          { Passability = Crushable;   return; }
//         if (t->Wall)                               goto DESTROYABLE;
//         if (GroundType[t->LandType].Cost[2] == 0)  { Passability = Impassable;  return; }
//         if (t->IsARock)                            { Passability = Impassable;  return; }
//         if (t->IsRubble)                           goto PASSABLE;
//     }
//     lt = cell->LandType;
//     if (lt == LandType_Water) { Passability = Water; return; }
//     if (lt == LandType_Beach) { Passability = Beach; return; }
//     if (GroundType[lt].Cost[2] <= 0.01) { Passability = Impassable; return; }
//     if (!cell->FirstObject) goto PASSABLE;
//     ... object walk: WhatAmI 6 (building) / 36 (terrain), tube and snow cases ...
// DESTROYABLE: Passability = Destroyable; return;
// PASSABLE:    Passability = Passable;
//
// The object walk is unreachable here: the generator never attaches an object
// to a cell, so FirstObject is always null and control falls straight through
// to Passable - which is also the answer the walk gives for an empty list.
// ---------------------------------------------------------------------------
void RandomMapGenerator::RecalcCellPassability(MapCell* cell)
{
    if (cell == nullptr)
        return;

    const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);

    if (!IsWithinUsableArea(CellStruct{ x, y }, true))
    {
        cell->Passability = PassabilityType_OutsideMap;
        return;
    }

    const int overlayIndex = cell->OverlayTypeIndex;
    if (overlayIndex >= 0
        && overlayIndex < static_cast<int>(overlayTypes_.size()))
    {
        const OverlayTypeInfo& ov =
            overlayTypes_[static_cast<size_t>(overlayIndex)];

        if (ov.crushable)
        {
            cell->Passability = PassabilityType_Crushable;
            return;
        }
        if (ov.wall)
        {
            cell->Passability = PassabilityType_Destroyable;
            return;
        }
        if (GroundTypeFor(ov.landType).cost[2] == 0.0f)
        {
            cell->Passability = PassabilityType_Impassable;
            return;
        }
        if (ov.isARock)
        {
            cell->Passability = PassabilityType_Impassable;
            return;
        }
        if (ov.isRubble)
        {
            cell->Passability = PassabilityType_Passable;
            return;
        }
    }

    const int landType = cell->LandType;
    if (landType == 2)                                 // LandType_Water
    {
        cell->Passability = PassabilityType_Water;
        return;
    }
    if (landType == 6)                                 // LandType_Beach
    {
        cell->Passability = PassabilityType_Beach;
        return;
    }
    if (GroundTypeFor(landType).cost[2] <= 0.01f)
    {
        cell->Passability = PassabilityType_Impassable;
        return;
    }

    // FirstObject is always null during generation.
    cell->Passability = PassabilityType_Passable;
}

// ---------------------------------------------------------------------------
// SetupLAT - CellClass::SetupLAT (0x47CA80 - 0x47D209, size 0x78A).
//
// Runs from RecalcAttributes before the floor's land class is decided. When the
// cell's tile belongs to one of the LAT ("look-alike transition") families it
// rewrites the tile: it walks the four orthogonal neighbours (Neighbours
// directions 0, 2, 4, 6 = N, E, S, W) and builds a 4-bit mask of "that
// neighbour is NOT one of us". A family's sixteen pieces are exactly the sixteen
// neighbour patterns, so the new tile is familyBase + mask; a mask of 0
// (surrounded by our own kind) falls back to the family's plain base tile.
//
// Four of the six blocks are that mask pattern, each with its own family pair
// and its own "also counts as ours" set. Each block does the same three steps:
// load the plain tile, accept the cell when it equals the plain tile or lies in
// [base, base + 15], then store `base + mask`, or the plain tile when the mask
// came out 0. The pairs below are the game globals the disassembly actually
// loads, with the address of each block's plain-tile load as its anchor:
//
//   block  anchor    plain tile              transition base          also "ours"
//     1    0x47cb9e  RoughTile  0xABC2B8     ClearToRoughLat 0xAA1134  -
//     2    0x47cc54  SandTile   0xABB104     ClearToSandLat  0xAA0E24  -
//     3    0x47cd1f  GreenTile  0xAA0E18     ClearToGreenLat 0xAA0748  ShorePieces
//                                                                      0xABAD28 +41,
//                                                                      WaterBridge
//                                                                      0xAA1090 +1
//     4    0x47ce00  PaveTile   0xABC2B0     ClearToPaveLat  0xAA10A8  MiscPaveTile
//                                                                      0xAA10A4 +13,
//                                                                      Medians
//                                                                      0xAA0E20 +13,
//                                                                      PavedRoads
//                                                                      0xABBEC8 +20
//   5. RampBase(20 wide, 0xABC1D8) / RampSmooth(12 wide, 0xAA1058) - NOT a
//      mask: picks a slope piece for SlopeIndex 1..4 by testing two diagonal
//      neighbours.
//
// The [General] key that names the plain tile and the key that names its
// sixteen-piece transition set are never adjacent lines of the INI - the
// pairing is RoughTile 13 / ClearToRoughLat 14, SandTile 33 / ClearToSandLat 34,
// GreenTile 41 / ClearToGreenLat 42, PaveTile 46 / ClearToPaveLat 39 - and
// MMRampBase and MonorailSlopes are not read here at all. Pairing a plain tile
// with a transition set from another material rewrites ground into unrelated
// families: sand came out as pavement, and every GreenTile came out as a
// 2..17 "ZMM Ramps" (mslop) wedge, since MMRampBase resolves to 2.
//
// Block 1's anchor goes through CellStruct::GetFoundationMapCrd (0x42D510),
// which is a plain coordinate add, so every block indexes neighbours alike.
//
// The tail makes sure the resulting tile's image is loaded. Vanilla's return
// value ("the tile is no longer GreenTile") is dropped by the caller.
// ---------------------------------------------------------------------------
void RandomMapGenerator::SetupLAT(MapCell* cell)
{
    // The eight Neighbours offsets (0x89F688): N NE E SE S SW W NW.
    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    if (cell == nullptr)
        return;

    const int16_t cx = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int16_t cy = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);

    // A neighbour by Neighbours index. Off-map reads give the sentinel cell,
    // whose tile index is 0 and so never matches a family.
    const auto at = [&](int dir) -> MapCell*
    {
        const int16_t nx = static_cast<int16_t>(cx + kDirX[dir]);
        const int16_t ny = static_cast<int16_t>(cy + kDirY[dir]);
        if (!CellExists(nx, ny))
            return &invalidCell_;
        return CellAt(nx, ny);
    };

    // One mask-style block. `plain` is the plain tile of the material, `base`
    // the family base and `span` its last piece. Each extra pair is another
    // family that also counts as "ours" (blocks 3 and 4 need those).
    const auto lat = [&](int base, int span, int plain,
                         std::initializer_list<std::pair<int, int>> extraOwn)
    {
        if (base == -1)
            return;
        const int cur = cell->IsoTileTypeIndex;
        if (cur != plain && (cur < base || cur > base + span))
            return;

        const auto ours = [&](int t)
        {
            if (t == plain || (t >= base && t <= base + span))
                return true;
            for (const std::pair<int, int>& own : extraOwn)
            {
                if (own.first != -1 && t >= own.first && t <= own.first + own.second)
                    return true;
            }
            return false;
        };

        int mask = 0;
        for (int n4 = 0; n4 < 4; ++n4)
        {
            const int dir = (n4 * 2) & 7;              // 0, 2, 4, 6 = N, E, S, W
            if (!ours(at(dir)->IsoTileTypeIndex))
                mask |= 1 << n4;
        }
        cell->IsoTileTypeIndex = mask ? base + mask : plain;
    };

    lat(clearToRoughLatIndex_, 15, roughTileIndex_, {});          // 0xABC2B8 / 0xAA1134
    lat(clearToSandLatIndex_, 15, sandTileIndex_, {});            // 0xABB104 / 0xAA0E24
    lat(clearToGreenLatIndex_, 15, greenTileIndex_,               // 0xAA0E18 / 0xAA0748
        { { shorePieces_, 41 }, { waterBridgeIndex_, 1 } });
    lat(clearToPaveLatIndex_, 15, paveTileIndex_,                 // 0xABC2B0 / 0xAA10A8
        { { miscPaveTileIndex_, 13 },
          { mediansIndex_, 13 },
          { pavedRoadsIndex_, 20 } });

    // ---- 5. RampBase / RampSmooth: slope piece selection (0x47cc80) --------
    if (rampBaseIndex_ != -1 && rampSmoothIndex_ != -1)
    {
        const int cur = cell->IsoTileTypeIndex;
        if ((cur >= rampBaseIndex_ && cur <= rampBaseIndex_ + 19)
            || (cur >= rampSmoothIndex_ && cur <= rampSmoothIndex_ + 11))
        {
            int mask = 0;
            if (cell->SlopeIndex == 1)                 // W, NE
            {
                mask = (at(6)->SlopeIndex == 0) ? 1 : 0;
                if (at(1)->SlopeIndex == 0)
                    mask |= 2;
                if (mask)
                    cell->IsoTileTypeIndex = mask + rampSmoothIndex_ - 1;
            }
            if (cell->SlopeIndex == 2)                 // N, E
            {
                if (at(0)->SlopeIndex == 0)
                    mask |= 1;
                if (at(2)->SlopeIndex == 0)
                    mask |= 2;
                if (mask)
                    cell->IsoTileTypeIndex = mask + rampSmoothIndex_ + 2;
            }
            if (cell->SlopeIndex == 3)                 // NE, W
            {
                if (at(1)->SlopeIndex == 0)
                    mask |= 1;
                if (at(6)->SlopeIndex == 0)
                    mask |= 2;
                if (mask)
                    cell->IsoTileTypeIndex = mask + rampSmoothIndex_ + 5;
            }
            if (cell->SlopeIndex == 4)                 // E, N
            {
                if (at(2)->SlopeIndex == 0)
                    mask |= 1;
                if (at(0)->SlopeIndex == 0)
                    mask |= 2;
                if (mask)
                    cell->IsoTileTypeIndex = mask + rampSmoothIndex_ + 8;
            }
            if (mask == 0)
                cell->IsoTileTypeIndex = cell->SlopeIndex + rampBaseIndex_ - 1;
        }
    }

    // Tail (0x47d1f4): vanilla loads the new tile's image here. Our tile-cell
    // attribute table is index-addressed and loaded on demand, so there is no
    // step to mirror.
}

// ---------------------------------------------------------------------------
// RecalcAttributes - CellClass::RecalcAttributes (sub_47D2B0 @ 0x47D2B0, size
// 0xab4). This is the single call the "Recalculating cell attributes" stage
// makes per cell, and it owns everything the cell's appearance and passability
// depend on: the floor's land class, the LAT piece (SetupLAT), the ramp type,
// the Z adjust, and the passability record.
//
// Three entry shapes, in vanilla order:
//
//   A. the overlay is a Wall / Railroad / "NoUseTileLandType" one (0x47d2f0)
//      -> land class from the overlay; ores on it are wiped when the cell is a
//         ramp; the "cliff back" probe may rewrite the floor to Rock; then
//         SetupLAT + the passability pass and return.
//   B. the cell has a tile index (0x47d3xx)
//      -> if the tile has no frame for this cell, the whole cell is wiped
//         (tile 0xFFFF, height 0, land 0) and the passability pass runs;
//         otherwise SetupLAT, the overlay/ore reconciliation (sub_544BE0,
//         ToTiberiumIdx), the level override, and the Z adjust.
//   C. the cell has no tile (0x47d6xx)
//      -> land class from the overlay alone, then the same tail as B.
//
// The "cliff back" probe (six neighbours, all as "at least one level higher":
// N, W, (x+2, y+2), SE, SW and NE) only rewrites a floor when
// RulesClass->CliffBackImpassability == 2, the value the shipped rules INI
// uses.
//
// Not modelled, because the port has neither: the TubeClass branch for
// LandType_Tunnel (the generator never places tubes), the tile-animation attach
// (AnimClass), and the shadow-caster stamp that sets CellsFlags bit 0x10000.
// The last one changes only neighbouring cells' flags, which nothing in the
// port reads.
// ---------------------------------------------------------------------------
void RandomMapGenerator::RecalcAttributes(MapCell* cell, int cellLevel)
{
    if (cell == nullptr || cell == &invalidCell_)
        return;

    const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);

    const int passIndex = PassabilityIndex(x, y);
    CellLevelPassability* pass =
        (passIndex >= 0 && passIndex < static_cast<int>(levelAndPassability_.size()))
            ? &levelAndPassability_[static_cast<size_t>(passIndex)]
            : nullptr;

    // The overlay type behind cell->OverlayTypeIndex, or null when there is
    // none / it is out of range.
    const auto overlay = [&](int index) -> const OverlayTypeInfo*
    {
        if (index < 0 || index >= static_cast<int>(overlayTypes_.size()))
            return nullptr;
        return &overlayTypes_[static_cast<size_t>(index)];
    };

    // "Cliff back" probe (0x47d399 - 0x47d52d): is any of the six neighbours at
    // least one level higher (cell->Level + 4)? The offsets are vanilla's, the
    // odd (x+2, y+2) included.
    const auto cliffBack = [&](void) -> bool
    {
        if (cliffBackImpassability_ == 0)
            return false;
        const int higher = cell->Level + 4;

        static const int16_t kProbeX[6] = { 0, -1, 2, 1, -1, 1 };
        static const int16_t kProbeY[6] = { -1, 0, 2, 1, 1, -1 };
        for (int i = 0; i < 6; ++i)
        {
            const int16_t nx = static_cast<int16_t>(x + kProbeX[i]);
            const int16_t ny = static_cast<int16_t>(y + kProbeY[i]);
            if (!CellExists(nx, ny))
                continue;
            if (higher <= CellAt(nx, ny)->Level)
                return true;
        }
        return false;
    };

    const auto storePassability = [&](void)
    {
        if (pass != nullptr)
        {
            pass->cellLevel = static_cast<uint8_t>(cell->Level);
            pass->cellPassability = static_cast<uint8_t>(cell->Passability);
        }
    };

    const int overlayIndex = cell->OverlayTypeIndex;
    const OverlayTypeInfo* ov = overlay(overlayIndex);

    // ---- A. Wall / Railroad / NoUseTileLandType overlay (0x47d2f0) --------
    if (overlayIndex != -1 && ov != nullptr)
    {
        cell->LandType = ov->landType;

        if (ov->landType == 4 || ov->landType == 9 || ov->noUseTileLandType)
        {
            if (cell->IsoTileTypeIndex != 0xFFFF
                && cell->IsoTileTypeIndex >= 0
                && cell->IsoTileTypeIndex < static_cast<int>(tileCellAttrs_.size()))
            {
                cell->SlopeIndex = static_cast<uint8_t>(
                    TileCellRampType(cell->IsoTileTypeIndex, cell->Height));
            }
            if (cell->SlopeIndex != 0 && ov->tiberium)
            {
                cell->OverlayTypeIndex = -1;
                cell->OverlayData = 0;
            }
            if (cliffBack() && cliffBackImpassability_ == 2)
                cell->LandType = 3;                    // LandType_Rock

            SetupLAT(cell);
            RecalcCellPassability(cell);
            storePassability();
            return;
        }
    }

    // ---- the tile index is out of the tile array --------------------------
    if (cell->IsoTileTypeIndex >= static_cast<int>(tileCellAttrs_.size()))
        cell->IsoTileTypeIndex = 0xFFFF;

    // ---- C. no tile: land class from the overlay alone (0x47d6xx) ---------
    if (cell->IsoTileTypeIndex == 0xFFFF || cell->IsoTileTypeIndex < 0)
    {
        if (overlayIndex == -1 || ov == nullptr || ov->noUseTileLandType)
            cell->LandType = 0;                        // LandType_Clear
        else
            cell->LandType = ov->landType;
        cell->SlopeIndex = 0;
    }
    else
    {
        // ---- B. the cell has a tile (0x47d3xx) ---------------------------
        const int tile = cell->IsoTileTypeIndex;
        const int height = cell->Height;

        bool hasFrame = TileCellHasFrame(tile, height);
        if (cell->SlopeIndex != 0)
        {
            // A cell that is already a ramp piece also accepts the tile's
            // cell 1 (vanilla probes sub_544C20 with a2 = 1).
            hasFrame = hasFrame || TileCellHasFrame(tile, 1);
        }

        if (!hasFrame)
        {
            // The tile has no frame here: wipe the cell down to a bare floor.
            cell->IsoTileTypeIndex = 0xFFFF;
            cell->Height = 0;
            cell->LandType = 0;
            cell->SlopeIndex = 0;
            if (cliffBack() && cliffBackImpassability_ == 2 && cell->LandType == 0)
                cell->LandType = 3;

            RecalcCellPassability(cell);
            storePassability();
            return;
        }

        cell->SlopeIndex =
            static_cast<uint8_t>(TileCellRampType(tile, height));
        SetupLAT(cell);

        // ---- overlay / ore reconciliation (0x47d4xx LABEL_40) ------------
        int landType;
        const int ovIdx = cell->OverlayTypeIndex;
        if (ovIdx != -1 && ToTiberiumIdx(ovIdx) != -1)
        {
            const OverlayTypeInfo* oreOv = overlay(ovIdx);
            if (cell->SlopeIndex <= 4)
            {
                if (oreOv != nullptr && oreOv->landType == 0)   // Clear
                    landType = 5;                               // Tiberium
                else
                    landType = cell->LandType;
            }
            else
            {
                // A ramp under ore: the ore is wiped and the floor's own land
                // class (from the TMP terrain type) takes over.
                landType = TileCellLandType(tile, height);
                cell->OverlayTypeIndex = -1;
                cell->OverlayData = 0;
            }
        }
        else
        {
            const OverlayTypeInfo* plainOv = overlay(ovIdx);
            if (ovIdx != -1 && plainOv != nullptr && plainOv->noUseTileLandType)
                landType = cell->LandType;
            else
                landType = TileCellLandType(tile, height);
        }
        cell->LandType = landType;

        // LandType_Tunnel would build a TubeClass here; the generator never
        // places tubes, so vanilla's branch cannot be reached.

        if (cellLevel != -1)
            cell->Level = cellLevel;

        int cellsInX = 0;
        int yAdjust = 0;
        if (TileCellImageInfo(tile, height, &cellsInX, &yAdjust))
            cell->ZAdjust = (yAdjust - 30) / 15;
        (void)cellsInX;                                // vanilla reads it too, but
                                                       // only ZAdjust is stored

        // The tile-animation attach and the shadow-caster stamp follow here in
        // vanilla; neither has a counterpart in this port (see the header).
    }

    // ---- LABEL_82: the shared tail ----------------------------------------
    if (cliffBack() && cliffBackImpassability_ == 2)
    {
        const int lt = cell->LandType;
        if (lt == 0 || lt == 2 || lt == 6 || lt == 8)   // Clear, Water, Beach, Ice
            cell->LandType = 3;                         // LandType_Rock
    }

    RecalcCellPassability(cell);
    storePassability();
}

// ---------------------------------------------------------------------------
// The "RMG: Recalculating cell attributes" stage (0x598e1f - 0x598e9e).
//
//     0x598e31  sub_578350                  CellIterator::Reset
//     0x598e3b  MapClass::CellIteratorNext   walk the diamond
//     0x598e48  sub_47D2B0(cell, -1)         once per cell
//     0x598e5b  psub_48D1D0()                progress-bar callback  (dropped)
//     0x598e6c  Scenario / Session tail                              (dropped)
//
// The identical walk runs three more times later in the flow - 0x598fb8 (after
// tiberium), 0x59912a (before the hills) and 0x599354 (after the LATs) - always
// with cellLevel = -1, meaning "do not change the level".
// ---------------------------------------------------------------------------
void RandomMapGenerator::RecalculateCellAttributes()
{
    if (cellSlots_ == nullptr)
        return;

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth);
    while (MapCell* cell = it.Next())
        RecalcAttributes(cell, -1);
}
