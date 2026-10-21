#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// ============================================================================
// Shared numeric helpers (used by MapGen.cpp and MapGenRiver.cpp)
//
// F2I64 - Game::F2I64 (0x7C5F00, RC=3): truncate toward zero. The double
//   overload must not narrow through float first - river coordinates live on
//   the x87 stack and the game truncates the st0 value directly.
// kUnitScale - Random_w scale (dbl_7ED898): 2^-32 * (1 + 2^-32), bits
//   0x3DF0000000100000. NOT 1/2^32 and NOT the nearest double of 1/(2^32-1) -
//   do not "fix".
// ============================================================================
inline int F2I64(float v)  { return (int)v; }
inline int F2I64(double v) { return (int)v; }
constexpr double kUnitScale = 2.3283064370807974e-10;

// ============================================================================
// LandType - 5 terrain layouts from RA2 RMG
//   0 = Archipelago
//   1 = Continent
//   2 = Team Continent
//   3 = Inland (special, no 1.2 clamp)
//   4 = Mountainous (special, no 1.2 clamp)
// ============================================================================
enum class RmLandType : int
{
    Archipelago   = 0,
    Continent     = 1,
    TeamContinent = 2,
    Inland        = 3,
    Mountainous   = 4,
};

// ============================================================================
// Theater type
// ============================================================================
enum class TheaterType : int
{
    Temperate = 0,
    Snow      = 1,
};

// ============================================================================
// Time of day
// ============================================================================
enum class TimeOfDay : int
{
    Morning   = 0,
    Afternoon = 1,
    Dusk      = 2,
    Night     = 3,
};

// ============================================================================
// Rect (RectangleStruct)
// ============================================================================
struct MapRectTag
{
    int X;
    int Y;
    int Width;
    int Height;
};

// ============================================================================
// CellStruct - packed cell coordinate (YRpp GeneralStructures.h)
//   struct CellStruct { __int16 X; __int16 Y; };   // 4 bytes
// Also the element type of the foundation-preview lists, which are
// terminated by the sentinel (0x7FFF, 0x7FFF).
// ============================================================================
struct CellStruct
{
    int16_t X;
    int16_t Y;
};

// ============================================================================
// FoundationPreviewState - the DisplayClass foundation-preview fields
// (YRpp DisplayClass.h) that the water-smoothing passes read and write.
//
//   vanilla offset   YRpp member name                    here
//   +0x1174          CurrentFoundation_CenterCell        CenterCell
//   +0x1178          CurrentFoundation_TopLeftOffset     TopLeftOffset
//   +0x117C          CurrentFoundation_Data              Data
//
// Vanilla keeps these in the process-global MouseClass::Instance. The
// generator owns its own copy because two steps of SmoothWaterBody touch it:
//   ResetPreviewState (sub_4A8BF0) clears it, and
//   SelectShoreTile    (sub_57ACF0) passes it to sub_4A91B0.
// Data points at a CellStruct list terminated by the (0x7FFF, 0x7FFF)
// sentinel; the list is limited to 120 entries (0x1E0 bytes).
// ============================================================================
struct FoundationPreviewState
{
    CellStruct  CenterCell;
    CellStruct  TopLeftOffset;
    CellStruct* Data;

    FoundationPreviewState()
        : CenterCell{0, 0}, TopLeftOffset{0, 0}, Data(nullptr)
    {}
};

// ============================================================================
// R250 Random Number Generator
//
// Exact replication of the game's global Randomizer (dst_ at 0xABE890):
//   - Seed  : sub_65C6D0 (4-round constant mixing per buffer slot)
//   - Next  : Randomizer::Random  (0x65C780, dual-pointer XOR feedback)
//   - Range : Randomizer::RandomRanged (0x65C7E0, bitmask rejection sampling)
//   - Unit  : Randomizer::Random_w (0x598000, Next() * 1/(2^32-1))
//   - Gauss : sub_5980C0 (Marsaglia polar method, second value cached)
// ============================================================================
// [FA2sp] Pluggable INI source.
//
// The retail build reads RMGMD.INI / rulesmd.ini / artmd.ini / temperatmd.ini
// from disk with GetPrivateProfile*, which cannot see the copies the editor
// already parsed out of the game MIX. When the host installs one of these,
// every INI lookup goes through it instead and no file is touched.
// ============================================================================
class RmIniSource
{
public:
    virtual ~RmIniSource() {}

    // [section] key -> int, or `fallback` when absent. `file` is the logical
    // INI the caller asked for ("rulesmd.ini" / "artmd.ini" /
    // "temperatmd.ini"): one source object serves them all, so it has to know
    // which one is meant.
    virtual int GetInt(const char* file, const char* section, const char* key,
                       int fallback) const = 0;

    // [section] key -> text. Writes an empty string when absent.
    virtual void GetString(const char* file, const char* section, const char* key,
                           char* out, int outSize) const = 0;

    // Every section name of `file`, in any order.
    virtual void GetSectionNames(const char* file,
                                 std::vector<std::string>& out) const = 0;
};

// Free helpers so the call sites stay as terse as the Win32 ones. `path` is
// only used when no RmIniSource is installed.
int  RmReadInt(const char* section, const char* key, int fallback, const char* path);
void RmReadString(const char* section, const char* key, const char* fallback,
                  char* out, int outSize, const char* path);
void RmReadSectionNames(std::vector<std::string>& out, const char* path);

// ============================================================================
class R250Random
{
public:
    R250Random();

    // Seed the generator (exact replication of sub_65C6D0)
    void Seed(uint32_t seed);

    // Generate next 32-bit random integer (Randomizer::Random, 0x65C780)
    uint32_t Next();

    // Random integer in [min, max] (Randomizer::RandomRanged, 0x65C7E0).
    // Bitmask rejection sampling - consumes a variable number of Next() calls.
    int RandomRanged(int lo, int hi);

    // Random integer in [lo, hi] (sub_598030, float scaling + upper rejection)
    int RandomFloatRange(int lo, int hi);

    // Uniform double in [0, 1] (Randomizer::Random_w, 0x598000)
    double Unit();

    // 25% chance boolean (sub_599650: Random() * 2.328306437080797e-10 < 0.25)
    bool Chance25();

    // Normal-distributed double (sub_5980C0, Marsaglia polar method)
    double Gaussian();

private:
    uint32_t buffer_[250];
    int      index1_;
    int      index2_;
    bool     gaussHave_;    // sub_5980C0 cached flag
    double   gaussSaved_;   // sub_5980C0 cached second value
};

// MapCell::AltFlags bits - subset of YRpp AltCellFlags (GeneralDefinitions.h)
// that the generation path actually touches.
enum AltCellFlags
{
    // Set while a cell is covered by the building foundation currently being
    // previewed, cleared when the preview moves away (sub_4A95A0).
    AltCellFlags_ContainsBuilding = 0x2,
};

// ============================================================================
// MapCell - simplified CellClass (only fields used during generation)
//
// Cell reset loop of sub_599650 (0x599e86):
//   SlopeIndex = 0, Level = 4, IsoTileTypeIndex = 0, Height = 0,
//   OverlayTypeIndex = -1, OverlayData = 0
//
// AltFlags (CellClass::AltFlags) is not part of that reset loop - the game
// zeroes it when the map is created - but it is carried here because the
// foundation-preview bookkeeping (sub_4A95A0) writes it.
// ============================================================================
struct MapCell
{
    int  MapCoords;          // packed: low 16 bits = X, high 16 bits = Y
    int  Level;
    int  IsoTileTypeIndex;
    int  SlopeIndex;
    int  Height;
    int  OverlayTypeIndex;
    int  OverlayData;
    int  AltFlags;           // AltCellFlags bits (ContainsBuilding)

    // ---- fields the "Recalculating cell attributes" stage owns ----
    // Offsets below are the vanilla CellClass ones; they are kept here so the
    // stage's writes land somewhere the map already carries.
    int  LandType;           // cell +0xEC: LandType_* floor class
    int  Passability;        // cell +0x4C: PassabilityType
    int  TubeIndex;          // cell +0x116: tunnel index (-1 = none)
    int  ZAdjust;            // cell +0x11D: the render Z adjustment
    int  CellFlags;          // cell +0x140: the cell's own flags word
                             // (bit 0x20000 = the tile animation was attached)

    MapCell()
        : MapCoords(0), Level(4), IsoTileTypeIndex(0), SlopeIndex(0)
        , Height(0), OverlayTypeIndex(-1), OverlayData(0), AltFlags(0)
        , LandType(0), Passability(0), TubeIndex(-1), ZAdjust(0), CellFlags(0)
    {}
};

// ============================================================================
// WorkCell - 80-byte work array cell (global dword_ABED10)
//
// Byte-level init from sub_599650 (0x59a321 - 0x59a376):
//   bytes  0..3   = 0     (MapCoords, two zero words)
//   bytes  4..7   = 0     (uninitialized in game; zeroed here)
//   bytes  8..63  = 0
//   bytes 64..67  = -1    (int32, data[16])
//   bytes 68..73  = 0
//   byte  74      = 1     (flags byte)
//   bytes 75..79  = 0     (76..79 uninitialized in game; zeroed here)
//
// Linear index: X + workSide * Y  (global diamond coordinates)
// ============================================================================
struct WorkCell
{
    int data[20];  // 20 * 4 = 80 bytes

    WorkCell()
    {
        for (int i = 0; i < 20; ++i)
            data[i] = 0;
        data[16] = -1;
        Byte(74) = 1;
    }

    // Byte accessor for the flag bytes (e.g. +74 validity flag).
    unsigned char& Byte(int offset)
    {
        return reinterpret_cast<unsigned char*>(data)[offset];
    }
    unsigned char Byte(int offset) const
    {
        return reinterpret_cast<const unsigned char*>(data)[offset];
    }

    int& MapCoords() { return data[0]; }
    int  MapCoords() const { return data[0]; }
};

// ============================================================================
// CellIterator - exact replication of the game's diamond cell iterator
//
//   Reset       : sub_578350 (0x578350)
//   Next        : MapClass::CellIteratorNext (0x578290)
//
// State (from MouseClass/DisplayClass):
//   CellIterator_NextX / NextY : coordinates of the NEXT cell to return
//   CellIterator_CurrentY      : advance-branch calls remaining on diagonal
//   CellIterator_NextCell      : linear slot index into Cells.Items
//
// Iteration order: diagonals s = x+y in increasing order; within each
// diagonal, x increases / y decreases. The first cell returned is (1, W').
// Iteration terminates when the slot holds nullptr (outside the diamond).
// ============================================================================
class CellIterator
{
public:
    CellIterator() : slots_(nullptr), mapWidth_(0)
        , nextX_(0), nextY_(0), currentY_(0), slotIndex_(0) {}

    // Replicates sub_578350
    void Reset(MapCell* const* slots, int mapWidth)
    {
        slots_ = slots;
        mapWidth_ = mapWidth;
        nextX_ = 1;
        nextY_ = mapWidth;
        currentY_ = mapWidth - 1;
        slotIndex_ = 512 * mapWidth + 1;
    }

    // Replicates MapClass::CellIteratorNext; returns nullptr when done
    MapCell* Next()
    {
        int curY = currentY_;
        int oldSlot = slotIndex_;
        if (curY)
        {
            nextY_ -= 1;
            nextX_ += 1;
            currentY_ = curY - 1;
            slotIndex_ = oldSlot - 511;
        }
        else
        {
            int ny = nextY_;
            int nx = nextX_;
            nextX_ = ny;
            nextY_ = nx;
            if (((ny - mapWidth_ + nx - 1) & 1) != 0)
            {
                currentY_ = mapWidth_ - 1;
                nextY_ = nx + 1;
            }
            else
            {
                currentY_ = mapWidth_ - 2;
                nextX_ = ny + 1;
            }
            slotIndex_ = nextX_ + (nextY_ << 9);
        }
        return slots_[oldSlot];
    }

private:
    MapCell* const* slots_;
    int mapWidth_;
    int nextX_;
    int nextY_;
    int currentY_;
    int slotIndex_;
};

// ============================================================================
// MapGenConfig - generation parameters (from UI, key RandomMapGenerator fields)
// ============================================================================
// ============================================================================
// GlobalMapOptions - the values sub_596300 (the random-map dialog's "start
// generation" handler, WM_COMMAND id 0x621) rolls BEFORE the map itself is
// seeded. They come from the engine's global Randomizer (this_pRandomizer,
// 0x886B88), whose seed is GetTickCount() - so the default behaviour differs
// every run, exactly like the retail game. Feeding the seed the game prints
// ("Seed is %08x") reproduces a specific run.
//
// The roll order (0x59678f - 0x59683c):
//
//     RandomRanged(0, 100)     -> flag010 = (result < 50) ? 1 : 0
//     RandomRanged(1, 4)       -> n3          (1..3)
//     RandomRanged(0, 3)       -> value020
//     RandomRanged(0, 3)       -> value018
//     RandomRanged(0, 3)       -> value03C (stored into 0xABE03C and 0xABE040)
//     sub_597260(n3)           -> reads the dialog's parameters, consumes RNG
//     RandomRanged(0, 0xFFFF)  -> seed04C (the map's random-seed label)
//
// The map's own RNG (dword_ABE890) is seeded with the constant 0 elsewhere
// (0x58b770), so the generation chain itself is deterministic.
// ============================================================================
struct GlobalMapOptions
{
    int flag010;     // dword_ABE010
    int n3;          // n3            - picks the MovementZone table
    int value020;    // dword_ABE020
    int value018;    // dword_ABE018
    int value03C;    // dword_ABE03C / dword_ABE040

    // ---- the sub_597260 rolls (this[..] on the instance at 0xABDFD8) ----
    //
    // NOTE: sub_597260 draws them from `this_pRandomizer` (0x886B88), which is
    // the game's SHARED randomizer - BattleClass / BulletClass::Detonate /
    // DropPodLocomotionClass::Process and twenty more engine sites use the same
    // object. Its state when the RMG dialog runs therefore depends on everything
    // the session did before, so the vanilla's values here are NOT reproducible.
    // Only the ranges matter for the port.
    //
    // Two lower bounds are not .rdata tables but .data arrays, dword_ABED40 /
    // dword_ABED18 (read at 0x5972a9 / 0x5972c6). Both are all zero in the image,
    // have no writer anywhere in the binary, and the only instructions that touch
    // them are those two reads - so the bound is 0: verified, not assumed.
    //
    // IMPORTANT: xrefs_to is NOT a valid way to decide whether one of these has a
    // writer or a consumer. Instance fields are written with register-relative
    // stores (sub_597260: "mov [edi+70h], eax" at 0x5972f6) that leave no xref,
    // and read either the same way or absolutely. An earlier pass used xrefs,
    // wrongly concluded these fields were never written, and modelled them as the
    // constant 0.
    //
    // The NAMES are the preset INI's keys: sub_597A30 loads [RandomMap]
    // WaterAmount / Ruggedness / UrbanPresence / Accessibility / RegionSize /
    // TiberiumLayout / Vegetation / NumPlayers / Resources / Width / Height / Seed
    // straight into this[19]/[17]/[24]/[27]/[28]/[22]/[23]/[20]/[16]/[25]/[26]/[29]
    // (0x597a30 - 0x597cb2).
    //
    // Which of them the generation consumes:
    //   waterAmount    - the river / lake stages                        (this[19])
    //   ruggedness     - hill height field, tiberium growth, LAT, rocks (this[17])
    //   vegetation     - the LAT stage (sub_5A38C0 / 5A3AE0 / 5A4280)   (this[23])
    //   accessibility  - the Making-regions ramp gate read at 0x5907b8  (this[27]).
    //                    Not cosmetic: the gate also guards a 1..2 draw that
    //                    consumes the map RNG, so a wrong value desynchronises
    //                    every later draw.
    //   regionSize     - the region size gate read at 0x58ed89         (this[28])
    //   tiberiumLayout - the starting-point want formula, 0x594f49      (this[22])
    //   urbanPresence  - NOT read during generation: only the dialog's own
    //                    load / clamp / show helpers touch it (sub_597A30 and
    //                    friends). The port needs nothing from it.     (this[24])
    int waterAmount;     // this[19]  WaterAmount    0x82B0A8[n3]  .. 0x82B0BC[n3]
    int ruggedness;      // this[17]  Ruggedness     0x82B10C[n3]  .. 0x82B120[n3]
    int urbanPresence;   // this[24]  UrbanPresence  0            .. 0x82B0F8[n3]
    int accessibility;   // this[27]  Accessibility  0            .. 0x82B0D0[n3]
    int regionSize;      // this[28]  RegionSize     0x82B080[n3] .. 0x82B094[n3]
    int tiberiumLayout;  // this[22]  TiberiumLayout 0 .. 100
    int vegetation;      // this[23]  Vegetation     RMGVegetationMinimums/Maximums[n3],
                         //                          clamped to 0..100, lo = min(lo, hi)
    int seed04C;         // this[29]  Seed           0 .. 0xFFFF - the map's random-seed label
};

// ----------------------------------------------------------------------------
// One entry of the hill stage's scratch elevation grid (game global
// dword_B0B6EC, 8 bytes per entry, built by sub_6B2A70).
//
// A cell's four tile corners are the `corner` fields of the four grid points
//     e(x, y), e(x + 1, y), e(x + 1, y + 1), e(x, y + 1)
// - the order sub_6B4240 and sub_6B3850 read them (their dword loads at +0 are
// what the "four corner values" are; the entry's size and its +4 / +5 flag
// bytes are pinned by the disassembly of all three users).
// ----------------------------------------------------------------------------
struct ElevationEntry
{
    int     corner;    // +0: the grid point's corner height, in fifteenths
    uint8_t blocked;   // +4: this point must not be levelled
    uint8_t touched;   // +5: sub_6B3E60 has written a corner here
    uint8_t pad[2];    // +6
};

struct MapGenConfig
{
    RmLandType  landType;     // this[15]
    int        theater;      // this[14]
    int        timeOfDay;    // this[18]  (0-3)
    int        sizeSlider;   // this[25] = this[26]  (0-3)
    int        playerCount;  // this[20]  (2-8)
    int        oreDensity;   // ore density index
    bool       multiplayer;  // false = single-player map (.map), true =
                             // multiplayer map (.yrm). Only the writer reads it:
                             // the two layouts differ in the file extension and
                             // in the [Header] / [Basic] / [Houses] sections,
                             // see SaveMapFile.
    int        waterAmount;  // this[19]  water amount percent (0-100).
                            // Rolled by the UI layer per generation
                            // (sub_597260 @ 0x597282, per-LandType range
                            // tables 0x82B0A8 / 0x82B0BC), like the seed.
    uint32_t   randomSeed;   // this[29]  (dialog seed field). NOT used to
                             // seed the map RNG any more - vanilla seeds that with
                             // the constant 0 (0x58b770); see GenerateMapBody.
    uint32_t   mapRngSeed;   // 地形主体 RNG (rng_ / dword_ABE890) 的种子。
                             // 0 = 原版固定 0；非 0 = UI 调试输入的种子。

    // The engine-global options rolled before generation starts. n3 picks the
    // MovementZone table the starting-point stage uses, so it belongs here
    // rather than being rolled inside the generator. See GlobalMapOptions.
    GlobalMapOptions global;

    // [FA2sp] Explicit map extent, in the generator's MapRect form
    // (W' = visible width + 4, H' = visible height + 12). 0 = derive from
    // sizeSlider / playerCount as the retail dialog does.
    //
    // Needed because CalcMapSize is table-driven and can only produce 74..124,
    // which cannot cover an arbitrary editor selection when the generator is
    // used to paint a sub-region of an existing map.
    int overrideMapWidth = 0;   // W'
    int overrideMapHeight = 0;  // H'
};

// ============================================================================
// MapSizeResult - computed map dimensions
//
//   width/height   : visible size W x H (LocalSize = {2, 5, W, H})
//   mapWidth       : W' = W + 4   (MapRect.Width)
//   mapHeight      : H' = H + 12  (MapRect.Height)
//   workSide       : W' + H' + 1  (dword_89C2DC, sub_42AC00)
// ============================================================================
struct MapSizeResult
{
    int width;
    int height;
    int mapWidth;
    int mapHeight;
    int workSide;
};

// ============================================================================
// RadarImage - the product of the "RMG: Compute Radar Image" stage.
//
// Modelled on FinalAlert 2's minimap (CMapData::InitMinimap /
// GetMiniMapPos / Mini_UpdatePos), because FA2 is what the written .map gets
// opened in - see MapGenRadar.cpp for the full derivation.
//
//     width  = 2 * [Map] Width      (two radar pixels per cell)
//     height = [Map] Height
//
// The background is white, every cell paints its (tile, sub-tile) frame's TMP
// radar bytes as those two pixels, overlays and the starting points override,
// and cells FA2 has no art for stay white.
//
// pixels is 0xAARRGGBB, row-major, `width` pixels per row, TOP-DOWN - what a
// 32-bpp DIB with a negative height wants.
// ============================================================================
struct RadarImage
{
    int width;
    int height;
    std::vector<uint32_t> pixels;

    RadarImage() : width(0), height(0) {}

    bool Empty() const { return width <= 0 || height <= 0; }
};

// ============================================================================
// RMGSettings - all fields read from RMGMD.INI [General] (sub_5981F0)
// ============================================================================
struct RMGSettings
{
    int MinTiberium;   // RMGMinimumTiberium
    int MaxTiberium;   // RMGMaximumTiberium
    int MaxTrees;      // MaxTrees

    std::vector<int> LevelLightSettings;
    std::vector<int> TemperateAmbientLight;
    std::vector<int> SnowAmbientLight;
    std::vector<int> TemperateAmbientRed;
    std::vector<int> TemperateAmbientGreen;
    std::vector<int> TemperateAmbientBlue;
    std::vector<int> SnowAmbientRed;
    std::vector<int> SnowAmbientGreen;
    std::vector<int> SnowAmbientBlue;
    std::vector<int> VegetationMinimums;
    std::vector<int> VegetationMaximums;

    std::vector<std::string> TemperateOrePatchLamps;
    std::vector<std::string> SnowOrePatchLamps;

    void SetDefaults();
    int LevelLight(int timeOfDay) const;
    int AmbientLight(int theater, int timeOfDay) const;
    int VegetationMin(int landType) const;
    int VegetationMax(int landType) const;
};

// ============================================================================
// MapRegion - one region record of the RMG region system
//
// Vanilla keeps 0x50-byte region objects in the global array dword_ABDF94
// (count dword_ABDFA0); sub_58BF70 constructs them and sub_5AC290 removes them.
// Fields are added as the stage that uses them is replicated; the offsets below
// are the vanilla ones (Init regions = sub_58C800 / sub_58CF90 / sub_58D010,
// Making regions = sub_58EBC0 / sub_58EF10 / sub_5A19E0 / sub_578E60 /
// sub_5A17F0).
//
//   vanilla offset  field         meaning
//   +0x04           neighbours    pointer to the DynamicVectorClass<int> that
//                                 sub_58F0C0 builds and sub_58EF10 frees: the ids
//                                 of the regions touching this one. Modelled as a
//                                 plain vector; sub_58BF70 leaves it empty (the
//                                 ctor stores 0 at +0x04).
//   +0x08           id            region id, also stamped into work[+56]
//   +0x0C           cellCount     number of cells the region covers
//   +0x10           level         the Level the region sits on
//   +0x14           waterFamily   byte: 1 when the seed cell is a water / shore
//                                 family tile
//   +0x1A           mergeSettled  byte: the "Making regions" merge-pass flag.
//                                 0 = still to be settled; sub_58EBC0 settles
//                                 the small non-water regions by writing 1
//                                 (0x58eded) and sub_58D620 refuses to split a
//                                 region that already carries 1 (0x58d631).
//                                 sub_58BF70 initialises it to 0 (0x58bfc1) and
//                                 AssignRemainingCells stamps its argument -
//                                 0, as the stage calls it - over it (0x58d055).
//   +0x1B           rampFlag      byte: 1 from the constructor (0x58bfc4), cleared
//                                 by sub_5905D0 when it could not carve a single
//                                 ramp towards a neighbour (0x590a4f, `*(this+27)
//                                 = 0`).
//   +0x28           cells         the embedded DynamicVectorClass<Cell> at
//                                 +0x28 (vtable +0x28, Items +0x2C, Capacity
//                                 +0x30, IsInitialized byte +0x34, IsAllocated
//                                 +0x35, Count +0x38, Growth +0x3C): the packed
//                                 coords of every cell the region covers.
//                                 sub_58BF70 constructs it empty, sub_58EBC0's
//                                 first loop clears it (vtable +0x0C call) and
//                                 then refills it, and sub_58C800 appends the
//                                 cells it grows. Modelled as a plain vector;
//                                 the clear / free are the vector's own.
//   +0x24           consumed      byte: set by SplitOrDropRegion when the record
//                                 it created was handed over to a neighbouring
//                                 region (0x58e4eb). SplitOrDropRegion's own tail
//                                 and the phase-5 skip both test it (0x58e52e /
//                                 0x58dfaf); sub_58BF70 initialises it to 0
//                                 (0x58be0e area: `*(this + 36) = 0`).
//   +0x40           bounds        the bounding rect sub_58EBC0 grows over the
//                                 same cells: {minX, minY, width, height},
//                                 reset to {9999, 9999, 0, 0} per region before
//                                 the rescan (0x58ec17 / 0x58ec98).
// ============================================================================
struct MapRegion
{
    // +0x00 the pick list sub_594870 leaves behind: the starting-point cells
    //   sub_594F40 chose for this region, with the ones actually stamped as
    //   waypoints removed from the front, so the leftovers stay for whatever
    //   wants them next. The vanilla keeps the vector OBJECT here (releasing the
    //   previous one through its vtable before storing a new one); the port
    //   keeps a plain vector.
    std::vector<CellStruct> startPointPicks;
    std::vector<int> neighbours;  // +0x04 the neighbour-region id vector
    int  id;           // +0x08
    int  cellCount;    // +0x0C
    int  level;        // +0x10
    bool waterFamily;  // +0x14 (byte)
    unsigned char mergeSettled;  // +0x1A (byte)
    unsigned char rampFlag;      // +0x1B (byte)
    int  startingPoints;         // +0x20 how many starting points this region was given
    unsigned char consumed;      // +0x24 (byte)
    std::vector<CellStruct> cells;  // +0x28 embedded DynamicVectorClass<Cell>
    MapRectTag bounds;              // +0x40 { minX, minY, width, height }
};

// ============================================================================
// OverlayTypeInfo - one entry of the port's OverlayTypeClass stand-in
//
// CellClass::RecalcAttributes (sub_47D2B0) reads exactly three fields off the
// overlay type it finds in cell->OverlayTypeIndex:
//     LandType            - 0..11, the LandType_* enum
//     Tiberium            - "this overlay is ore"
//     NoUseTileLandType   - "ignore the tile's land type"
// OverlayTypeClass::LoadFromINI (0x5FE770) fills them from the rules INI, and
// then rewrites LandType_Clear to LandType_Tiberium whenever Tiberium is set:
//     if (Tiberium && LandType == Clear) LandType = Tiberium;
// which is how [GEM01] ("Tiberium=yes", its "Land=" commented out) ends up as
// LandType_Tiberium.
//
// The index -> name mapping comes from the rules INI's [OverlayTypes] section.
// The list has gaps, so an index the file never names stays `present == false`
// and behaves like LandType_Clear with both flags off.
// ============================================================================
struct OverlayTypeInfo
{
    bool present;               // named by [OverlayTypes]
    int  landType;              // LandType_* (0..11)
    bool tiberium;              // "Tiberium="
    bool noUseTileLandType;     // "NoUseTileLandType="

    // The four more fields sub_483C80 (the passability pass) reads off the
    // overlay: "Crushable" comes from the base ObjectTypeClass::LoadFromINI
    // (0x5F92D0), the other three from OverlayTypeClass::LoadFromINI (0x5FE770).
    bool crushable;             // "Crushable="
    bool wall;                  // "Wall="
    bool isARock;               // "IsARock="
    bool isRubble;              // "IsRubble="

    OverlayTypeInfo()
        : present(false), landType(0), tiberium(false)
        , noUseTileLandType(false), crushable(false), wall(false)
        , isARock(false), isRubble(false)
    {}
};

// ============================================================================
// PassabilityType - CellClass::Passability (YRpp GeneralDefinitions.h:729, same
// order as the engine's enum).
// ============================================================================
enum PassabilityType
{
    PassabilityType_Passable     = 0,
    PassabilityType_Crushable    = 1,
    PassabilityType_Destroyable  = 2,   // tree or wall
    PassabilityType_Beach        = 3,
    PassabilityType_Water        = 4,
    PassabilityType_HasFreeSpots = 5,
    PassabilityType_Impassable   = 6,
    PassabilityType_OutsideMap   = 7,
};

// ============================================================================
// CellLevelPassability - one entry of the two parallel "cell attributes" arrays
//
// Vanilla keeps them on MouseClass::Instance:
//     LevelAndPassability              (CellLevel + CellPassability)
//     LevelAndPassabilityStruct2pointer
// and addresses both through sub_56D3F0, whose index is
//     x + y * (W' + H' + 1)   clamped into [0, count)
// i.e. exactly the work array's own indexing. RecalcAttributes ends every path
// by copying the cell's Level into both arrays and its Passability into the
// first one.
// ============================================================================
// YRpp's CellLevelPassabilityStruct (MapClass.h:33) is FOUR bytes, not three
// ints:
//     +0  char            CellPassability
//     +1  char            CellLevel
//     +2  unsigned short  ZoneArrayIndex
// The layout matters: the zone rebuild walks this array with a 4-byte stride and
// clears the third byte with `*(i + 2) = 0`, and GetMoveError indexes
// MovementZones with ZoneArrayIndex.
struct CellLevelPassability
{
    uint8_t  cellPassability;    // +0
    uint8_t  cellLevel;          // +1
    uint16_t zoneArrayIndex;     // +2
};

// ============================================================================
// MapStructure - one entry of the map's building list.
//
// The "B" route for the objects the generator places (so far only the bridge
// repair hut of sub_5904B0): instead of instantiating a live BuildingClass and
// threading it onto the cell's object chain, the port appends a record here. The
// fields are exactly what the .map [Structures] section needs - the type's INI
// name, the cell, the owner and the facing - so the output stage can be added
// later without touching the generation code.
//
// The cell additionally carries AltCellFlags_ContainsBuilding, which stands in
// for the side effect vanilla's Unlimbo has (a non-empty object chain): the
// placer of sub_5904B0 must not drop a second hut on an already occupied cell.
// ============================================================================
struct MapStructure
{
    CellStruct  coords;
    const char* typeName;    // the [BuildingTypes] name, e.g. "CABHUT"
    const char* houseName;   // the owner section, e.g. "Neutral House"
    int         facing;      // DirType_* (North = 0)

    MapStructure()
        : coords(), typeName(nullptr), houseName(nullptr), facing(0)
    {}

    MapStructure(CellStruct cell, const char* type, const char* house, int face)
        : coords(cell), typeName(type), houseName(house), facing(face)
    {}
};

// ============================================================================
// MapTerrainObject - one entry of the map's terrain-object list.
//
// The "B" route for the terrain objects sub_5A28C0 creates on an ore field
// (the ore decoration trees). Vanilla builds a live 0xE0-byte object and places
// it with two engine calls:
//     index = sub_71DD80("TIBTRE0n");       // name -> type array index
//     sub_71BB90(obj, GlobalTypeArray[index], &coords);
// The port has no object system, so the placement is recorded here instead -
// exactly what the .map [Terrain] section needs (the type's INI name and the
// cell). Only three names exist: TIBTRE01 / TIBTRE02 / TIBTRE03.
// ============================================================================
struct MapTerrainObject
{
    CellStruct  coords;
    const char* typeName;    // "TIBTRE01" .. "TIBTRE03"

    MapTerrainObject()
        : coords(), typeName(nullptr)
    {}

    MapTerrainObject(CellStruct cell, const char* type)
        : coords(cell), typeName(type)
    {}
};

// ============================================================================
// StartingPointRecord - one entry of the port's starting-point ledger.
//
// The vanilla's starting-point stage writes the scenario state directly:
// ScenarioClass::Waypoints (through sub_68BF50), bit 4 of the cell's Flags
// word, and the eight dwords at ScenarioClass+0x11C0. The port has no
// ScenarioClass, so route B is used again: the same information is recorded
// here (and in `waypoints_`), which is what the .map writer will read for
// [Waypoints] and the start positions.
//
//   index   the waypoint number the entry was stamped as. sub_594B50 hands
//           sub_594870 a running offset, so the first starting point is
//           waypoint 0 - the first house slot - and they count up.
//   coords  the starting-point cell, i.e. the cell carrying Flags bit 4.
// ============================================================================
struct StartingPointRecord
{
    int        index;
    CellStruct coords;

    StartingPointRecord() : index(0), coords() {}
    StartingPointRecord(int idx, CellStruct cell) : index(idx), coords(cell) {}
};

// ============================================================================
// NeutralTechBuilding - one entry of RulesClass::NeutralTechBuildings.
//
// The list is [AI] NeutralTechBuildings in the rules INI (six type names
// in the shipped rulesmd.ini; RulesClass reads this list from the [AI]
// section, not [General]). sub_595400 picks one at random, creates the live
// BuildingClass for it and then looks for a region cell its foundation fits on.
// The port keeps the two things the stage needs: the name the .map [Structures]
// section wants, and the foundation rectangle the fit test walks.
//
// The rectangle comes from the art INI's Foundation=WxH for the type (see
// LoadNeutralTechBuildings), and the engine's own list is the very same shape.
// What the whole chain resolves to:
//   BuildingTypeClass::LoadFromINI  0x461532 - 0x461541
//       FoundationData (+0x0DFC) = 0x89C900 + Foundation * 120
//   -> the per-enum table at 0x89C900, 120 bytes (30 CellStructs) per slot:
//      the cells, the (0x7FFF,0x7FFF) terminator, then zero padding.
//   sub_45B1C0 fills that table, writing each foundation's cells ROW-MAJOR from
//      (0,0): 2x1 is {(0,0),(1,0)}, 2x2 is {(0,0),(1,0),(0,1),(1,1)}, ...
//   BuildingTypeClass::GetFoundationData  0x45EC20 (vtable + 0x90)
//      is a getter: it ignores its includeBib argument, answers the field, and
//      answers an empty list (nothing but the terminator) when the field is 0.
// So the foundation cells are base + (col, row) for col in [0, W) and
// row in [0, H), with the position cell at the rectangle's first cell - exactly
// what the port walks. The art INI's AddOccupy / RemoveOccupy keys feed the
// separate FoundationOutside field, not this list.
// ============================================================================
struct NeutralTechBuilding
{
    char name[32];      // the [BuildingTypes] name, e.g. "CAOILD"
    int  width;         // Foundation W (the art INI's "WxH", first number)
    int  height;        // Foundation H

    NeutralTechBuilding() : width(1), height(1) { name[0] = '\0'; }
};

// ============================================================================
// MultiplayerHouse - one entry of the map's [Houses] ledger in the multiplayer
// (.yrm) layout, i.e. one country of the rules INI's [Countries] section.
//
// FA2's multiplayer branch (CHouses::OnPreparehouses, Houses.cpp:253-284) walks
// rules [Countries] and, for every country, registers the country itself as a
// map house and fills its property section:
//     [Houses]      <index>=<country>
//     [<country>]   IQ=0 / Edge=North / Color=<rules [<country>] Color> /
//                   Allies=<country> / Country=<country> / Credits=0 /
//                   NodeCount=0 / TechLevel=1 / PercentBuilt=0 /
//                   PlayerControl=no
// Note a multiplayer map carries [Houses] only - no [Countries] section; the
// shipped .yrm files match. The single-player layout is the other way round:
// [Houses] registers "<country> House" and [Countries] is written as well.
// ============================================================================
struct MultiplayerHouse
{
    char country[32];   // the rules [Countries] value, e.g. "Americans"
    char color[24];     // the rules [<country>] Color, e.g. "Gold"

    MultiplayerHouse() { country[0] = '\0'; color[0] = '\0'; }
};

// ============================================================================
// TileCellAttr - one cell of one isotile's TMP image
//
// CellClass::RecalcAttributes reaches four helpers that all answer a question
// about "cell Height of tile T", and every one of them reads the tile's TMP
// image:
//     sub_544C20(tile, 0, Height)  does that cell carry an image frame?
//     sub_544BE0(tile,    Height)  its LandType (via the terrain-type table)
//     sub_5471B0(tile,    Height)  its ramp type
//     sub_547150(tile,    Height, &cellsInX, &yAdjust)
// The TMP layout LoadFromFile (0x547020) reads is:
//     +0   int32 CellsInX
//     +4   int32 CellsInY
//     +8   int32 cellWidth
//     +12  int32 cellHeight
//     +16  int32 offset[CellsInX * CellsInY]   file offset of each cell's data
// and the per-cell data (extra) carries:
//     +41  byte  terrain type (0..15) -> LandType through kLandTypeFromTerrain
//     +42  byte  ramp type
//     +43  byte  radar red   left    +46  byte  radar red   right
//     +44  byte  radar green left    +47  byte  radar green right
//     +45  byte  radar blue  left    +48  byte  radar blue  right
// A zero offset means the cell has no frame at all.
//
// The six radar bytes are the ones FA2's minimap reads verbatim
// (XCC_GetTMPTileInfo's lpRgbLeft / lpRgbRight, MissionEditorPackLib.cpp:631):
// every map cell paints TWO radar pixels from its (tile, sub-tile) frame, the
// left one from +43..+45 and the right one from +46..+48.
// ============================================================================
struct TileCellAttr
{
    bool          hasFrame;      // the TMP offset entry was non-zero
    unsigned char terrainType;   // extra[+41]
    unsigned char rampType;      // extra[+42]
    unsigned char flags36;       // extra[+36], read as a dword by sub_547150
    int           cellsInX;      // the TMP header, repeated per cell (sub_547150)
    int           cellsInY;      // the TMP header, repeated per cell (sub_547150)
    int           extra4;        // extra[+4]  - sub_547150's Y adjustment
    int           extra24;       // extra[+24] - sub_547150's Y adjustment

    // The frame's radar pixels; see above. All zero when the TMP header is too
    // short to carry them.
    unsigned char radarRedLeft;
    unsigned char radarGreenLeft;
    unsigned char radarBlueLeft;
    unsigned char radarRedRight;
    unsigned char radarGreenRight;
    unsigned char radarBlueRight;
};

// ============================================================================
// GroundTypeInfo - one entry of GroundType::Array (0x89EA40, 12 entries)
//
// YRpp MapClass.h:14 spells it { float Cost[8]; bool Buildable; }, filled from
// the rules INI's per-floor sections - the twelve section names are exactly the
// LandType strings:
//     [Clear] [Road] [Water] [Rock] [Wall] [Tiberium]
//     [Beach] [Rough] [Ice] [Railroad] [Tunnel] [Weeds]
// Each section carries one percentage key per speed type (Foot, Track, Wheel,
// Float, Hover, Amphibious, FloatBeach) plus Buildable. Cost[2] is the WHEEL
// column - the one sub_483C80 tests to decide whether a cell can be crossed.
// ============================================================================
struct GroundTypeInfo
{
    float cost[8];              // speed-type multipliers, in SpeedType order
    bool  buildable;

    GroundTypeInfo()
    {
        for (int i = 0; i < 8; ++i)
            cost[i] = 0.0f;
        buildable = false;
    }
};

// ============================================================================
// TiberiumInfo - one entry of TiberiumClass::Array
//
// rulesmd.ini lists the ore types in [Tiberiums] (Riparius, Cruentus, Vinifera,
// Aboreus) and each one's own section carries an "Image=" NUMBER, which
// TiberiumClass::LoadFromINI (0x721A50) turns into a hard-coded overlay type:
//     Image = 2 -> OverlayTypeClass::Array[27]     (the gem floor)
//     Image = 3 -> OverlayTypeClass::Array[127]
//     Image = 4 -> OverlayTypeClass::Array[147]
//     Image = anything else, 1 included -> Array[102]
// and then sets NumImages = 12, NumFrames = 12 and the +0xEC field = 8
// (0x721cc6 `mov dword ptr [esi+0ECh], 8`).
//
// ToTiberiumIdx (0x5FDD20) walks this table asking whether an overlay index
// falls in [overlayStart, +NumImages) or in the +0xEC-long block right after
// it, and answers the ore's own index when it does.
// ============================================================================
struct TiberiumInfo
{
    int overlayStart;   // Image->ArrayIndex, the hard-coded overlay above
    int numImages;      // 12
    int extra;          // the +0xEC field, 8
    int arrayIndex;     // the ore's own index - what ToTiberiumIdx returns
};

// ============================================================================
// RandomMapGenerator - terrain generation body
//
// GenerateMapBody replicates the RNG-relevant core of sub_599650:
//   1. rng_.Seed(seed)          (sub_598960: qmemcpy(&dst_, Seed(this[29])))
//   2. ReInitMapData()          (sub_5981F0, no RNG consumption)
//   3. CalcMapSize()            (0x599665 - 0x59976a)
//   4. InitCells()              (CreateEmptyMap semantics, 512-stride slots)
//   5. InitWorkArray()          (0x59a2fa - 0x59a37f, 80 * workSide^2 bytes)
//   6. FillWorkCoords()         (0x59a385 - 0x59a3dc, iterator order)
//   7. random25Flag_            (0x59a4cc, exactly ONE Random() call)
//
// Verified via xrefs of dst_ (0xABE890): no other RNG consumer runs between
// the seeding in sub_598960 and sub_59A6C0 terrain generation.
// ============================================================================
// One footprint cell outcome of a CliffSet piece stamped during PlaceCliffs
// (genCode == -1). ok == 0 means the cell already carried a cliff-family tile
// (a CliffRamp or an earlier CliffSet piece) and was skipped / aborted past.
struct CliffStampOutcome
{
    int16_t ox, oy;      // piece stamp origin
    int16_t x, y;       // footprint cell coords
    uint8_t slot;       // 1..40 CliffSet slot of the piece
    uint8_t h;          // footprint h index (row * w + col)
    int8_t  z;          // piece z offset (4 = wall, 0 = foot)
    uint8_t ok;         // 1 stamped, 0 blocked
};

class RandomMapGenerator
{
public:
    RandomMapGenerator();
    ~RandomMapGenerator();

    bool GenerateMapBody(const MapGenConfig& cfg);

    // Re-initialize map data from RMGMD.INI (replicates sub_5981F0)
    //
    // [FA2sp] Only reads the file when s_bLoadRmgmdIni is true (default false:
    // the editor extension feeds the settings from its own dialog instead).
    bool ReInitMapData();

    // [FA2sp] When false, ReInitMapData() just calls settings_.SetDefaults()
    // and no RMGMD.INI is touched. The standalone tool sets this to true.
    static bool s_bLoadRmgmdIni;

    // [FA2sp] Folder holding rulesmd.ini / artmd.ini / temperatmd.ini /
    // snowmd.ini, with a trailing backslash. Empty = next to the executable.
    // The host points this at the game directory, which is where FA2 loaded
    // them from, so every INI lookup resolves to the same file the editor used.
    static std::wstring s_gameIniDir;

    // Resolve one of the game's INI files. Tries s_gameIniDir first, then the
    // standalone tool's candidate list (exe folder, one and two levels up, the
    // project's 相关INI folder). False when none exists.
    static bool ResolveGameIni(const wchar_t* fileName, wchar_t* out, int outSize);

    // [FA2sp] When set, all INI reads go through this instead of the disk.
    static RmIniSource* s_iniSource;

    // [FA2sp] Push the settings collected by the host UI. Must be called before
    // GenerateMapBody(): RollGlobalOptions already reads VegetationMin/Max.
    // (The getter already exists further down, next to the other accessors.)
    void SetSettings(const RMGSettings& s) { settings_ = s; }

    // ---- [FA2sp] host data injection -------------------------------------
    // When s_bUseHostData is true, GenerateMapBody skips the six Load* passes
    // (theater INI walk, TMP parsing, rules INI reads) and uses whatever the
    // host pushed through the setters below. The editor extension feeds FA2's
    // already-parsed theater/rules tables and TMPs pulled out of the game MIX,
    // so nothing is loaded from disk at generation time.
    static bool s_bUseHostData;

    // [FA2sp] Parse one TMP from a memory buffer. Same layout the file loader
    // uses, so the host can pull the bytes out of a MIX and reuse this parser.
    // An empty `out` means "no image" - the cell-attribute pass treats it so.
    static void ParseTmpBuffer(std::vector<TileCellAttr>& out,
                               const unsigned char* data, int size);

    void SetTileCellAttrs(std::vector<std::vector<TileCellAttr> > v)
    {
        tileCellAttrs_.swap(v);
    }
    void SetOverlayTypes(std::vector<OverlayTypeInfo> v) { overlayTypes_.swap(v); }
    void SetGroundTypes(std::vector<GroundTypeInfo> v) { groundTypes_.swap(v); }
    void SetTiberiums(std::vector<TiberiumInfo> v) { tiberiums_.swap(v); }
    void SetNeutralTechBuildings(std::vector<NeutralTechBuilding> v)
    {
        neutralTechBuildings_.swap(v);
    }
    void SetMultiplayerHouses(std::vector<MultiplayerHouse> v)
    {
        multiplayerHouses_.swap(v);
    }

    // Theater tile-family indices (IsometricTileTypeClass::ReadINI, 0x545150).
    // Reads the [General] keys ShorePieces / WaterSet / CliffSet / GreenTile /
    // ClearToGreenLat / WaterfallEast / WaterfallWest / WaterfallSouth /
    // WaterfallNorth from TEMPERATMD.INI (theater 0) or SNOWMD.INI (theater 1)
    // next to the executable into shorePieces_, waterTileIndex_,
    // shoreTileIndex_, greenTileIndex_, clearToGreenLatIndex_ and
    // waterFamily4Base_. Missing keys or a missing file leave -1.
    void LoadTheaterTiles(int theater);

    // Regular terrain generation (replicates sub_59A6C0).
    // Segment 1 (0x59a724 - 0x59a738): flood every diamond cell with the
    // water base tile (game global nIdx = WaterSet).
    // Segment 2 (0x59a6f6 - 0x59a718): dispatch to the land type generator.
    // Segments 3-6 (isolated water fill, sub_57A0C0 smoothing, work array
    // reset, coast grass pass) are NOT implemented.
    void GenerateTerrain();

    // ---- Segment 2 land type generators ----
    // Common skeleton shared by all three (see sub_59BBC0_注释.md):
    //   genCode_ = 1;                 // this[194] = 1
    //   ClearWorkOccupancy();         // work data[15] = 0, whole map
    //   target = <per generator formula>;
    //   loop {
    //       seed = <per generator rule>;
    //       grown = GrowPatch(area, bounds, seed, 1, center, step, mode);
    //       if (grown) { ++genCode_; accumulated += grown; }
    //       pick next seed;
    //   } until target reached / attempt cap;
    void GenerateArchipelago();    // sub_59AD10: sub_59A8F0 block grid, step 0.25, mode 0
    void GenerateContinent();      // sub_59AFA0: single ellipse,          step 0.75, mode 1
    void GenerateTeamContinent();  // sub_59B200: two halves,              step 0.75, mode 1

    // Archipelago block-grid split (sub_59A8F0): cut `rect` into exactly
    // `count` rectangles packed in a ragged sqrt(count) x sqrt(count) grid;
    // the finer-split axis is chosen by a coin flip and the leftover cells are
    // merged back along the other axis. Appends the rects to `blocks`.
    void SplitVisibleRectIntoBlocks(std::vector<MapRectTag>& blocks,
                                    int count,
                                    const MapRectTag& rect);

    // Common generator prologue: iterate all cells in diamond order and
    // clear the work occupancy mark (data[15], byte 60). Verbatim prologue
    // of sub_59AD10 (0x59ad5e) / sub_59AFA0 (0x59b043) / sub_59B200 (0x59b063).
    void ClearWorkOccupancy();

    // Organic patch growth engine (sub_59BBC0) - full annotated walkthrough in
    // sub_59BBC0_注释.md. IMPLEMENTED.
    //   area         target cell count (forced >= 400 internally)
    //   bounds       boundary rect; nullptr = sentinel (unlimited)
    //   seedXY       packed seed (low16 X, high16 Y); (0,0) = random pick
    //   boundsMode   sub_59BAB0 a5: 0 = AABB, 1 = ellipse
    //   centerXY     packed region center for directional priority
    //   step         gaussian walk step a7 (0.25 archipelago / 0.75 continent)
    //   priorityMode a8: 0 = radial distance + noise, 1 = directional (sub_59B940)
    // Returns cells grown (seed + main loop + leftover queue sweep), 0 on
    // failure (map rolled back, caller retries with another seed).
    int GrowPatch(int area, const MapRectTag* bounds, int seedXY,
                  int boundsMode, int centerXY, double step, int priorityMode);

    // sub_59BAB0 - the patch's boundary test. boundsMode 0 walks a plain AABB,
    // 1 walks an ellipse: the diamond coords are rotated into screen space and
    // accepted while u^2 * (1/(W/2)^2) + v^2 * (1/(H/2)^2) < 1. `bounds` equal
    // to the all-zero sentinel (nullptr here) accepts everything.
    //   0x59b0.. call site: sub_59BAB0(&neighbour, rect, a5, v98, v97)
    //   invHalfWidthSq / invHalfHeightSq are the two precomputed reciprocals.
    // IMPLEMENTED.
    bool PatchInBounds(int cellXY, const MapRectTag* bounds, int boundsMode,
                       double invHalfWidthSq, double invHalfHeightSq) const;

    // sub_59B940 - priority mode 1: the direction-weighted distance used by the
    // continent / team-continent generators. Takes the region centre -> cursor
    // unit vector in screen space, scales it by the two rect aspect weights and
    // multiplies by the Chebyshev distance from the candidate to the cursor.
    // With the all-zero sentinel rect it degenerates to the plain Euclidean
    // distance. No RNG.
    //   0x59c1ab call site: sub_59B940(&cursorCell, &neighbour, rect, centre,
    //                                   v86, v85)
    // IMPLEMENTED.
    double PatchDirectionalPriority(int cursorXY, int cellXY,
                                    const MapRectTag* bounds, int centreXY,
                                    float weightA, float weightB) const;

    // Special terrain generation (replicates sub_59C580).
    // Vanilla dispatch (sub_598960 @ 0x598aed - 0x598b0d):
    //   if (this[15] == 3 || this[15] == 4) {   // Inland / Mountainous
    //       if (this[19])                       // water amount nonzero
    //           sub_59C580(this);
    //   }
    //   else
    //       sub_59A6C0(this);                   // -> GenerateTerrain()
    // Unlike sub_59A6C0 (flood all water, then grow land), the LandType 3/4
    // base map stays all-land (cell reset leaves IsoTileTypeIndex = 0), so
    // this generator carves the special features (water / lake patches) -
    // the inverse role. Full annotated walkthrough: sub_59C580_注释.md.
    // Implemented: prologue (preparation, ++genCode_) + river stage dispatch
    //              (up to 10 GenerateRiver tries, first success stops) +
    //              river start / angle / width / length initialization
    //              (sub_59D510 head, 0x59d541 - 0x59d925) + the lake stage
    //              (10 tries, first success bumps the generation code).
    void GenerateSpecialTerrain();

    // Water-detail pass (replicates sub_59C630). The main flow calls it
    // unconditionally right after the terrain dispatch (sub_598960 @
    // 0x598b14), i.e. for every land type - the 3/4 special-terrain path
    // included. Walks the diamond and, for every plain open-water cell
    // (IsoTileTypeIndex == waterTileIndex_ and Height == 0) whose E / S / SE
    // neighbours are plain water too, either swaps in a random water variant
    // tile or drops a water detail tile. Consumes 1-2 RNG draws per such cell.
    // Implemented: gates, RNG draws and the variant branch. Pending: the
    // detail-tile stamp - PlaceWaterDetailTile (sub_5A6C10).
    void DecorateWaterTiles();

    // Init-regions stage (replicates the "RMG: Init regions" stage of
    // sub_598960, 0x598C24 - 0x598D42; its debug string sits at 0x82BED8 and is
    // referenced only from 0x598C24). Runs for every land type, right after the
    // water-detail pass, and does:
    //   1. reset the work region marks (work[+56] = -1, work[+60] = -1)
    //   2. tear the previous region objects down
    //   3. reset the region-id counter (dword_ABED14)
    //   4. seed regions from the water-family / green-ground cells
    //   5. grow the body of every water region
    //   6. assign the cells step 4 left unassigned
    // Implemented: the stage body and every subordinate it calls -
    // ReleaseRegionObject, SeedRegionsFromWaterCells, the shared builder
    // BuildRegionFromCell, CollectRegionBoundary, GrowRegionBody,
    // SweepRegionBody and AssignRemainingCells.
    // Decompile: the sub_598960 slice in 598C24_InitRegions.c, with 58CF90.c /
    // 58D010.c / 5AC290.c / 58E740.c / 58E9B0.c / 58C800.c / 58D410.c /
    // 58BF70.c / 4867B0.c / 5AC370.c / 42F7C0.c.
    void InitRegions();

    // Making-regions stage (replicates the "RMG: Making regions" stage of
    // sub_598960, 0x598D44 - 0x598E1E; its debug string sits at 0x82BEEC and is
    // referenced only from 0x598D44). Runs right after InitRegions and before
    // the "Recalculating cell attributes" stage. Body:
    //   if (landType == 4 || landType == 3)          // 0x598d58-0x598d60
    //   {
    //       sub_58EBC0();                            // rebuild bodies + settle
    //       sub_58EF10();                            // re-seed + adjacency + ramps
    //       sub_5A19E0(this);                        // cliff edge Level fix-up
    //       sub_578E60(0, -1);                       // cliff placement driver
    //       sub_5A17F0(this);                        // cliff correction pass
    //   }
    //   sub_59B740(this);                            // 0x598e1a, every land type
    // The UI / session / psub_48D1D0 tail of the vanilla stage (0x598d87 -
    // 0x598e18) is deliberately not ported - it touches neither the work array,
    // the region array, the cells nor the RNG, and this tool only generates
    // maps. Decompile: 598D44_MakingRegions.c and the call-tree files it lists.
    void MakeRegions();

    // The "RMG: Recalculating cell attributes" stage (0x598e1f - 0x598e9e): walk
    // the diamond calling RecalcAttributes(cell, -1). The same stage runs three
    // more times later in the flow (after tiberium, before the hills, after the
    // LATs).
    void RecalculateCellAttributes();

    // MapClass::GetMoveError (0x56d230) - the movement cost of a cell for one
    // MovementZone: MovementZones[zone][LevelAndPassability[idx].ZoneArrayIndex],
    // with idx clamped into the map's valid range. The bridge case (the last
    // argument true) is not reachable from the generator.
    int GetMoveError(CellStruct coords, int movementZone) const;

    // sub_56C510 (0x56c510 - 0x56cb8d) - the movement-zone rebuild: resets the
    // five MovementZones tables, flood-fills a zone number into every cell
    // (sub_56CB90), then merges touching zones through MapClass::ZoneConnections
    // / sub_56CB90's recording object and writes each cell's ZoneArrayIndex.
    // The return value is the index the reference zone's cost table starts at.
    int RebuildMoveZones();

    // sub_56CB90 (0x56cb90 - 0x56cfff) - the recursive scanline fill. `row`
    // points at one cell of levelAndPassability_ (four bytes per cell: [0]
    // passability-then-zone number, [1] level, [2..3] ZoneArrayIndex). Every cell
    // of the run is stamped with `zone` and every border with another zone is
    // recorded in zoneConnections_. `out` receives how many cells this call
    // covered; the return value is the run length the caller accumulates.
    int FillZoneFromRow(uint8_t* row, int zone, int* out);

    // One zoneConnections_ push: record that `zone` and `other` touch. The
    // vanilla skips the write when the pair is already present.
    void RecordZonePair(int zone, int other);

    // Roll the engine-global options the dialog handler produces before
    // generation starts (see GlobalMapOptions). `seed` is the global
    // Randomizer's seed - GetTickCount() by default, or the value the game
    // logged as "Seed is %08x" to reproduce a specific run.
    GlobalMapOptions RollGlobalOptions(uint32_t seed);

    // The "RMG: Creating starting points" stage (0x598e9e - 0x598ebe): a retry
    // loop over its two steps, run until both answer true.
    void CreateStartingPoints();

    // 出生点阶段内部：放完点立刻校验所有出生点同处一个"可通行+同高程+正交连通"
    // 平面，不满足就把全部点收进最大同高平面重放。不动 CreateHills。
    void EnsureStartingPointsConnected();

    // [port-only] EnsureStartingPointsConnected 的陆路连通版，只在
    // Continent(1)/Inland(3)/Mountainous(4) 三种陆地型上使用。组件允许跨高程：
    // 同高直接相连，差一档必须经真实斜坡瓦（SlopeIndex 非 0），差两档及以上
    // 或穿水/崖不连通。出生点可位于同一陆块的不同台面上（崖上/崖下），只要
    // 彼此有斜坡陆路可达。成功重放返回 true；无法在一个陆块内放下全部玩家时
    // 返回 false，由调用方回退到旧的同高平面逻辑。
    bool RelocateStartingPointsLandConnected(int players);

    // [port-only] EnsureStartingPointsConnected 的大岛屿群（TeamContinent）专用版，
    // 只在 RmLandType::TeamContinent(2) 上使用。这一型**不做陆地连通要求**、也不要求
    // 同一高程平面，只有两条规则：①出生点平均分摊到两块最大的陆地上（奇数时排在
    // 前面的岛多一个）；②沿用大岛屿的靠海约束（陆路 kCoastalSteps 步内必须能走到
    // 外海）。岛内仍按几何最远对 + 贪心最远拉开距离。两块岛用与陆路连通版完全相同的
    // 陆军口径 4 向泛洪找出，取格子数最大的两块。任一步凑不齐（陆块不足两块、岛内
    // 找不到足够的靠海平地）返回 false，由调用方回退到旧的同高平面逻辑。
    bool RelocateStartingPointsTwoIslands(int players);

    // [port-only] EnsureStartingPointsConnected 的群岛（Archipelago）专用版。
    // 规则：一岛一点 —— 陆地按陆军口径泛洪成一块块岛，按格子数从大到小取前
    // 玩家人数那么多块，每块放**恰好一个**出生点（岛比玩家人数多时多出来的
    // 不管）。**不要求靠海**，也不做陆地连通/同高平面要求。岛内锚点沿用各路径
    // 共用口径（可通行 + 自身平 + 6x6 同高净空 + 可见区内 + 离崖 4 格），严格
    // 口径找不到时依次放宽，尽量保证每岛都有点。岛数少于玩家人数时返回 false
    // 交调用方回退。
    bool RelocateStartingPointsOneIslandEach(int players);

    // [port-only] True when no ramp tile, impassable cliff facade or cell whose
    // Level differs by 2+ lies within `radius` orthogonal steps of `anchor`.
    // Keeps a starting point (and its build-out area) a flat distance away
    // from cliffs; both start-point relocation paths share this test.
    bool IsCliffClearAround(CellStruct anchor, int radius);

    // [port-only] Walkable-land step count from every walkable cell to the open
    // sea, indexed by work index (x + workSide * y). Sea cells are 0, land cells
    // that touch the sea are 1, -1 marks a cell the shore cannot be walked to
    // from. Only the water body connected to the map rim counts as sea, so
    // land-locked lakes and rivers are excluded. Keeps continent starting
    // points within kCoastalSteps of the coast.
    std::vector<int> BuildCoastalDistance();

    // The "RMG: Adding tech buildings" stage (0x598EBF - 0x598EE4): one call to
    // sub_5A95B0, gated so it is skipped only for LandType_Archipelago (0). Two
    // entirely different paths by land type; see MapGenTech.cpp.
    void AddTechBuildings();

    // The "RMG: Creating tiberium" stage (0x598EE5 - 0x598FB7): sub_5A23A0
    // grown ore / gem fields per region, then the region-system teardown (clear
    // the work marks, free every region, reset the id counter). See
    // MapGenTiberium.cpp.
    void AddTiberium();

    // [port-only] 群岛矿物阶段的矿柱（TIBTRE）规则：不再由矿田随手撒，而是
    // "每个出生点持有的矿柱数量完全相同"。先从 1..5 随机定一个目标数 N，再对各
    // 出生点所在岛的可用格数取最小值兜底，保证每个出生点都放得下 N 个；随后每个
    // 出生点在自己岛上取离它最近、且优先落在矿石上的 N 个格子各放一个 TIBTRE。
    // GrowTiberiumField 在群岛因此不再生成 TIBTRE。
    void PlaceArchipelagoOrePillars();

    // The "RMG: Creating hills" stage (0x599171 - 0x599214): sub_5A35F0's six
    // step sequence. No land-type gate. The stage body is in place; all six
    // steps are still stubs - see MapGenHills.cpp.
    void CreateHills();

    // The "RMG: Creating LATs, rocks etc" stage (0x599215 - 0x599353): four
    // calls plus one inline loop. No land-type gate. The stage body is in
    // place; every step is still a stub - see MapGenLAT.cpp.
    void CreateLATs();

    // The "RMG: Cleanup" stage (0x5993A5 - 0x59944C): releases the work array,
    // every region object and the per-cell attribute arrays, and resets the
    // region-id counter. No cell, tile or RNG is touched; see MapGenCleanup.cpp.
    void Cleanup();

    // The "RMG: Compute Radar Image" stage (0x599451 - 0x599477): rebuilds the
    // client's radar / display state for the finished map - no cell, tile, overlay
    // or RNG is touched. A documented no-op in the port (no MouseClass / radar);
    // see MapGenRadar.cpp.
    void ComputeRadarImage();

    // The last stage, "RMG: Done" (0x59947E - 0x59951A): the 100% progress tick,
    // the "RMG: Done" banner and the teardown of the local progress bar. The port
    // has a UI instead of a dialog, so it is implemented as the local path: the
    // 100% tick, then the slot reset plus the sink detach; see MapGenDone.cpp.
    void Done();

    // ---- the output side ---------------------------------------------------
    // Writes the finished map as a ".map" in the format the engine itself saves
    // through MapClass's writer (sub_4AD7E0 @ 0x4AD7E0) - IsoMapPack5 from the
    // cells, OverlayPack / OverlayDataPack from their overlays, plus the object
    // ledgers. This is not a vanilla stage; see MapGenMapFile.cpp for the format
    // and for how each part was verified against a shipped map.
    bool SaveMapFile(const wchar_t* path);

    // 产物输出目录：.map/.yrm、radar_preview.png 和 rmg_diag.log 全部写这里。
    // 内部始终保存成"带结尾反斜杠的绝对路径"；默认是 exe 所在文件夹，界面上
    // 的"浏览…"按钮可以让使用者改成任意文件夹。生成开始前调用一次即可。
    void SetOutputDir(const std::wstring& dir);
    const std::wstring& GetOutputDir() const;

    // [port-only diagnostic] Write a FA2-readable ".map" snapshot of the map in
    // its current state to Mapoutput, named "<YYYYMMDD_HHMMSS>_<stageName>.map".
    // Call it right after a pipeline step to diff that step in the editor; the
    // packed sections are the real compressed ones (SaveMapFile does the write).
    void SaveStageSnapshot(const char* stageName);

    // [TEMP DIAG] Shore bare-contact investigation. Writes one ASCII line to
    // Mapoutput\rmg_diag.log. Remove the whole diagnostic once located.
    //
    // [FA2sp] Default OFF. Under the editor extension these calls used to hit
    // GetModuleFileNameW + CreateDirectoryW + fopen on every one of the 100+
    // call sites, which is slow and drops rmg_diag.log next to FinalAlert.
    // Set true only when reproducing a generation bug.
    static bool s_bDiagLogEnabled;
    static void DiagLog(const char* fmt, ...);

    // Step 1 of that stage - sub_594B50 (0x594B50). Picks the regions the
    // starting points sit in; see MapGenStartpoint.cpp for the phases.
    bool SelectStartingPointRegions();

    // sub_594420 - flood one region out of `seedCell`: stamps the region id into
    // the work marks, grows the cell list, and only takes a neighbour whose
    // move error for `movementZone` still equals `referenceError`.
    void SeedRegionFromCell(MapCell* seedCell, int movementZone, int referenceError);

    // 出生点圈地的放宽开关：false=原版严格口径（只收移动代价等于最大分量的格）；
    // true=只要求纯可通行平地(Passability=0)，忽略移动分量差异。仅当严格口径连
    // 一个达到最低格数的地块都圈不出来时，SelectStartingPointRegions 才打开它重圈。
    bool startpointRelaxedMove_ = false;

    // sub_594870 - stamp the starting-point cells of one region: picks up to
    // `region->startingPoints` candidates by random 6x6 sampling, records the
    // chosen ones in waypoints_ / startingPoints_ (see the ledger members) and
    // marks their cells with CellFlags bit 4. The leftovers stay on the region.
    // The vanilla writes ScenarioClass instead; see StartingPointRecord.
    void StampStartingPointCells(MapRegion* region, int offset);

    // sub_594F40 - which of sub_594870's candidates become starting points:
    // `2 * region->startingPoints` of them (see the note in MapGenStartpoint.cpp
    // for the two globals the formula reads), spread out by the farthest-point
    // rule. No RNG.
    std::vector<CellStruct> ChooseStartingPointCells(
            const MapRegion* region,
            const std::vector<CellStruct>& candidates);

    // sub_595400 - the zero-share fallback: pick a random entry of
    // RulesClass::NeutralTechBuildings, then try up to 100 times to drop it on
    // a random cell of the region, answering true when one attempt worked.
    // The vanilla creates a live BuildingClass and Unlimbos it; the port takes
    // the same route B as the bridge repair hut and appends a MapStructure
    // (owner "Neutral", facing North) instead. Callers ignore the answer.
    bool GiveRegionATechBuilding(MapRegion* region);

    // The foundation fit test of sub_595400's inner loop (0x595548 - 0x5955e5):
    // every cell of the W x H rectangle at `base` must carry no object, a
    // placeholder tile, the anchor's Level, no Rock floor, a usable-area
    // position and a clear work byte +69.
    bool FoundationFitsAt(CellStruct base, int level, int width, int height);

    // [port-only] FoundationFitsAt plus a flat apron: every cell of the
    // (width + 2*apron) x (height + 2*apron) rectangle around the foundation
    // must be an in-diamond placeholder at the same Level. Region cliff ramps
    // are carved BEFORE tech buildings are placed, so the vanilla test alone
    // can accept a site wedged right against an existing ramp; this rejects
    // those and keeps one flat row between the building art and the nearest
    // slope. apron=0 is exactly the vanilla test.
    bool FoundationFitsAtFlat(CellStruct base, int level,
                              int width, int height, int apron);

    // Route B placement: append the building to structures_ and stamp every
    // cell of its W x H foundation with AltCellFlags_ContainsBuilding - the
    // stand-in for the object chain vanilla's Unlimbo fills on EVERY foundation
    // cell. Shared by GiveRegionATechBuilding (sub_595400) and the free
    // placement loop of AddTechBuildings (sub_5A95B0).
    void RecordPlacedBuilding(CellStruct base, const char* typeName,
                              int width, int height);

    // sub_5A28C0 - grow one ore / gem field around `seed` until `target` cells
    // carry ore, stamping work byte +0x3C (data[15]) with `mark`. `variant`
    // picks the overlay family (true -> 27 = gem floor, false -> 102 = ore
    // floor) and whether the TIBTRE decoration is recorded. See
    // MapGenTiberium.cpp.
    void GrowTiberiumField(CellStruct seed, int target, int mark, bool variant);

    // ---- Creating hills (sub_5A35F0) - all still stubs, see MapGenHills.cpp ----
    // Step 1 sub_5A33F0: for every shore / cliff-family tile, mark the first
    // placeholder neighbour (+) and seed the shore ones' height field with 0.5.
    void MarkHillProtection();
    // [port-only] Tech buildings are placed BEFORE the hills stage, and the
    // vanilla hill code only keeps the foundation cells themselves fixed
    // (CanHostSlope's FirstObject clause). That leaves every neighbour free to
    // be raised or carved, so a hill can ring a tech building with ramp tiles -
    // visually the building sits in a slope crater. This step paints the hill
    // protection mark (work byte +69) on every tech-building foundation cell
    // AND on a flat apron around it, forcing the height field to stay at ground
    // level there and pushing the hill's slope band outside the apron.
    void MarkTechBuildingProtection();
    // Step 2 sub_5A2F50: build the height field (work +8) and the amplitude
    // field (work +16); the stage's only RNG consumer.
    void BuildHillHeightField();
    // Step 3 sub_6B2A70: allocate and seed the scratch elevation grid over
    // MouseClass's MapCoordBounds.
    void BuildElevationGrid();
    // Step 4 (inline at 0x5A361A): walk the work array and drive
    // sub_6B4100 / sub_6B4240 / sub_6B3E60 to raise or lower each cell to the
    // height field's target.
    void ApplyElevationLevels();
    // Step 5 sub_6B3850: turn each cell's four grid corners into its Level, its
    // SlopeIndex and its tile (rampBaseIndex_ + slope - 1, or clearTileIndex_).
    void FinalizeElevationSlopes();
    // Step 6 (inline at 0x5A36CB): collapse the 2x2 blocks whose slopes are
    // 5/6/7/8 or 11/12/9/10 into the 0xFFFF multi-cell-tile mark.
    void MergeSlopeBlocks();
    // Port-only final sweep (runs after MergeSlopeBlocks): the micro-slopes
    // FinalizeElevationSlopes lays down did not exist when PlaceCliffs ran, so
    // a CliffSet piece can end up with a raised wall cell (L8) diagonally
    // beside an L4..L6 ramp cell, its facade riding across the ramp. Any such
    // piece is taken back to t0 (art only; Level / SlopeIndex untouched), so
    // the ramp stays open and the game loader fills the placeholder.
    void RemoveCliffsOverRamps();

    // ---- Creating LATs, rocks etc (0x599215) - all still stubs, see MapGenLAT.cpp ----
    // Step 1 sub_5A38C0: the three per-cell density doubles (+0x18/+0x20/+0x28),
    // written from this[23] * 0.01 for shore cells only.
    void PrepareLATDensities();
    // Step 2 sub_5A3AE0: LAT blobs on the placeholder cells, a SetupLAT refresh,
    // the tree patches and the overlay-168..177 tail.
    void ScatterLATBlobs();
    // Step 3 (inline at 0x59923E): write 0.001 into work +0x20 and 0.005 into
    // work +0x28 for every cell.
    void ResetLATDensityFields();
    // Step 4 sub_5A4280: rock blobs on the flat unprotected placeholder cells,
    // then the same tree-patch loop.
    void ScatterRocks();
    // sub_5A4B60: best-first growth of a LAT / rock blob of `count` cells,
    // stamping work dword +0x3C with the seed's slot index.
    void PlaceBlob(MapCell* cell, int tileIndex, int count, int slotIndex,
                   bool allowLatToLat, bool avoidStartClear = false);
    // sub_5A45E0: plant up to `count` TREE01..TREE25 objects around the cell,
    // returning how many were planted. Route B: records MapTerrainObject.
    int ScatterTrees(MapCell* cell, int count, double probability);
    // The green patch loop the vanilla inlines TWICE, once at the end of
    // sub_5A3AE0 (0x5a3f6c - 0x5a40bd) and once at the end of sub_5A4280
    // (0x5a4433 - 0x5a45cf), with identical code. Factored here so the two
    // cannot drift apart.
    void PlaceGreenPatches();

    // [移植侧] 第 4 次属性重算之后、写盘前，按最终格属性剔除放错位置的树
    // （TREE01..25，不含 TIBTRE 矿树）：水面格、真悬崖墙格（CliffSet /
    // WaterCliffs）、以及 0xFFFF 多格片覆盖格上的树一律删除；坡（RampBase /
    // CliffRamps / RampSmooth）上的树保留。同时让出生点周围少放树。
    void PruneTerrainTrees();

    // [移植侧] 格点是否落在任一出生点的净场 6x6 内（出生点 2x2 居中、外扩
    // 2 格）。出生点在 AddTiberium / CreateLATs 之前已确定，树、岩石生成时
    // 用它避让，而不是事后改地形。
    bool IsStartClearArea(int x, int y) const;
    // [移植侧] 矿石/矿柱净场 10x10（出生点 2x2 居中、外扩 4 格）。
    bool IsOreClearArea(int x, int y) const;

    // RulesClass::NeutralTechBuildings (rules INI [AI]) plus each type's
    // art INI Foundation= - the data sub_595400 needs. Read once per map; an
    // absent rules INI leaves the list empty (see GiveRegionATechBuilding).
    void LoadNeutralTechBuildings();

    // The country list the multiplayer .yrm [Houses] ledger wants: the rules INI
    // [Countries] section in index order, plus each country's [<country>] Color.
    // Read once per map; an absent rules INI leaves the list empty, and the
    // writer then falls back to the neutral house alone (see MultiplayerHouse).
    void LoadMultiplayerHouses();

    // Step 2 of that stage - sub_5A1FB0 (0x5A1FB0). Paints each starting point's
    // surroundings (400-cell nearest-first flood from the waypoint).
    bool PaintStartingPointTerrain();

    // One river attempt (replicates sub_59D510). Implemented: start /
    // angle / width / length initialization (0x59d541 - 0x59d925) + the
    // main-loop skeleton - (1) center diamond check, (2) cross-section
    // carving (generation-code stamping, pollution on old-water contact),
    // (3) river-mouth delta trigger (six gates + GenerateDelta dispatch),
    // (4) center advance (cx += cos, cy -= sin), (5) tributary fork (1%
    // roll each step, gaussian angle window, recursive call whose return
    // value overwrites the alive flag), (6) angle jitter (steps > 5;
    // gaussian sigma pi/10, rejection-clamped into theta0 +/- pi/2),
    // (7) width jitter (gaussian sigma 0.5, clamped into the fixed
    // [w0 - w0/2, w0 + w0/2] window; only when w0 >= 2), (8) the 0.5%
    // natural-termination roll. Post-loop: exit gates + terminal lake
    // (5.5) and top-level post-processing (5.6: smoothing, lakeshore
    // ring expansion, shore tile correction) implemented, plus the canyon
    // branch, the finish expansion and the rollback (5.7-5.9) with both of
    // the canyon's primitives (FindCandidateCenter, ExpandWaterBody) landed -
    // the whole chain now consumes RNG as vanilla does.
    // Full annotated walkthrough: sub_59C580_注释.md §5.2 - §5.9.
    //   startXY      packed start coords (low16 X, high16 Y);
    //                (0,0) = automatic mode (random edge start), used by
    //                the top-level call; nonzero = specified start
    //                (tributary recursion from the river main loop)
    //   initialAngle initial flow direction theta0 in radians; only used
    //                by the specified-start branch (vanilla number_16)
    //   isTributary  false = top-level call; true = tributary recursion
    //                (vanilla a4: 0 = top, nonzero = tributary; tributaries
    //                get no delta, no canyon lift and no post-processing.
    //                A river that forks a tributary sets this flag on
    //                itself - asm 0x59dea5 - and is demoted the same way
    //                for the rest of its lifetime.)
    // Returns true on success (step count already added to the water-cell
    // budget), false on failure (map already rolled back to all-land).
    bool GenerateRiver(int* startXY, double initialAngle, bool isTributary);

    // One river-mouth delta fan (replicates sub_59E740) - IMPLEMENTED.
    // Called by the river main loop's delta trigger (0x59ddf5) after all
    // six gates pass; carves a fan of water downstream of the river end
    // and, on success, advances the river mouth center 12 cells along the
    // fan heading so the main channel continues past the fan.
    //   genCode     generation code stamped on the fan cells
    //   startXY     packed first cell of the terminal cross-section
    //   endXY       packed last cell of the terminal cross-section
    //   direction   fan heading: 0 = -y, 2 = +x, 4 = +y, 6 = -x
    //   success     receives the result (vanilla v74[0])
    //   centerX/Y   river mouth center (in/out, advanced on success)
    void GenerateDelta(int genCode, int startXY, int endXY, int direction,
                       bool* success, double* centerX, double* centerY);

    // One lake attempt (replicates sub_59C920) - IMPLEMENTED.
    // Called from the river exit phase (0x59e22c) when the river ends by
    // natural termination, seeded with the river-mouth cell; the top-level
    // lake stage (GenerateSpecialTerrain stage 2) uses the same routine
    // with its own random start points.
    // Budget gate, node block + min-heap, qualification sweep, auto-mode seed
    // sampling, gaussian area sampling, priority growth loop, heap drain, size
    // gates, smoothing / lakeshore expansion / shore-tile fix and the failure
    // rollback. Its two subordinates are landed as well:
    // ClearPreviousGeneration (sub_5A0410) and the heap sift-down (sub_5AD870).
    //   startXY     packed seed cell (low16 X, high16 Y); (0,0) = automatic
    // Returns true on success; at the river exit a failure clears the
    // river's success flag (0x59e235).
    bool GenerateLake(int* startXY);

    // Four full-map smoothing passes over one water body (replicates
    // sub_57A0C0). Repairs the cells stamped with genCode; all call sites
    // pass flag = 0. Returns false to fail the caller.
    // Implemented: body (work-array reset + four iterator passes sharing one
    // success flag), pass 1 (FloodFill), pass 2 (CleanupTile) and passes 3/4
    // (SelectShoreTile - decision logic plus placement tail, including the
    // sub_57B440 stamp for the families whose geometry is tabulated).
    bool SmoothWaterBody(int genCode, int flag);

    // [FIX] Staircase-bend fill. Before shore pieces are stamped, carve the
    // inner tip of every staircase river bend so a continuous piece (shore15
    // family) becomes selectable. Idempotent; no-op when no bend is present.
    void FillStaircaseBends(int genCode);

    // [FIX] Unsolvable shore-corner fill. 两种"2x2 岸片无解"的水形在铺岸片
    // 前把咬得最近的那个对角水角填回陆地（每处只填 1 格）：
    //   A 型 单对角岸格的拐角片展开侧整列/整行贴着正交深水墙；
    //   B 型 两个背靠背的纯对角岸格（NE-SW / NW-SE）选中互相重叠的拐角片。
    // 判据对原版 6 张山地图 0 命中。与 FillStaircaseBends 同阶段。
    void FillUnsolvableShoreCorners(int genCode);

    // Ring expansion / absorption of one water body (replicates
    // sub_5A0160, full body recovered). Grows the body carrying `genCode`
    // outward one ring per mode step: mode 1 = lakeshore, 2 = riverside,
    // 6 = large-scale absorption; centerX/Y + rangeX/Y delimit the working
    // rect (full map = 0, 0, 512, 512); flag != 0 also accepts cells of the
    // previous generation (genCode - 1) and stamps `level` on placeholder
    // cells. Returns false when the ring meets another generation's water.
    bool ExpandWaterBody(int genCode, int mode, int centerX, int centerY,
                         int rangeX, int rangeY, int flag, int level);

    // sub_5A0700 - ring 0 of the body carrying `genCode`: its boundary cells
    // (every body cell that has an in-diamond neighbour outside the body),
    // appended once per outside neighbour exactly as vanilla does.
    std::vector<CellStruct> BuildWaterRing(int genCode) const;

    // sub_4865D0 - true when the cell's tile belongs to the water / shore
    // families: [shorePieces_, +42), [waterTileIndex_, +14) and four 4-wide
    // families (the game globals nIdx_0 @0xAA073C, nIdx_1 @0xABB110,
    // nIdx_2 @0xAA1050, nIdx_3 @0xAA10A0 - all INI-filled locally).
    bool IsWaterFamilyTile(const MapCell* cell) const;

    // sub_4865B0 (0x4865B0) - the shore family test alone: the cell's
    // IsoTileTypeIndex is in [shorePieces_, shorePieces_ + 42). The narrower
    // sibling of IsWaterFamilyTile; its consumer is the hill stage's protection
    // pre-mark (sub_5A33F0).
    bool IsShoreTile(const MapCell* cell) const;

    // sub_4867B0 - "green ground" family test, the second seeding gate of the
    // Init-regions stage (used by sub_58CF90, sub_58C800 and sub_58E740). True
    // when the cell's IsoTileTypeIndex is
    //   dword_AA0E18                     the base of the GreenTile ground set
    //   or in [dword_AA0748, +16)        the ClearToGreenLat transition set
    // Both globals come from the theater INI's [General] keys of the same
    // names (see LoadTheaterTiles). NOTE: dword_AA0E18 is NOT the CliffSet
    // global - the CliffSet base lives at dword_AA1020 (= shoreTileIndex_).
    bool IsGreenGroundTile(const MapCell* cell) const;

    // sub_486790 - the ClearToSandLat family test the LAT stage's overlay tail
    // uses: the cell's IsoTileTypeIndex is in [clearToSandLatIndex_, +16).
    bool IsClearToSandLatTile(const MapCell* cell) const;

    // The four pave-family tile tests of sub_58F2C0 and its rectangle
    // predicates. Each base comes from the theater INI [General] key of the
    // same name (LoadTheaterTiles):
    //   sub_4866D0  IsPavedRoadTile     [pavedRoadsIndex_,    +15) "PavedRoads"
    //                                       game global dword_ABBEC8
    //   sub_4866F0  IsPavedRoadEndTile  [pavedRoadEndsIndex_, + 4) "PavedRoadEnds"
    //                                       game global dword_ABBEC4
    //   sub_486650  IsMiscPaveTile      [miscPaveTileIndex_,  +14) "MiscPaveTile"
    //                                       game global dword_AA10A4
    //   sub_486670  IsPaveTile          [paveTileIndex_,      +16) "PaveTile"
    //                                       game global dword_ABC2B0
    bool IsPavedRoadTile(const MapCell* cell) const;

    // The vanilla's "is this tile morphable" clause, read on an UNSIGNED index:
    //     tile < Array.Count || tile == 0xFFFF || Items[tile]->Morphable
    // so an unset index (0xFFFFFFFF) and anything out of range take the "OK"
    // branch and only a valid tile whose TileSet lacks Morphable fails.
    bool IsMorphableTile(int tile) const;

    // sub_6B2520 - can this cell host a slope? In the diamond, no overlay, no
    // object, work byte +69 clear and a morphable tile.
    bool CanHostSlope(int16_t x, int16_t y);

    // ---- the hills stage's level-apply helpers (sub_6B4100 / 6B4240 / 6B3E60
    //      / 6B3A80), all driven by ApplyElevationLevels ----
    // sub_6B4100: the cell's level according to the elevation grid.
    int  ElevationCurrentLevel(const MapCell* cell);
    // sub_6B4240: the four-corner raise/lower mask for one step.
    int  ElevationStepMask(const MapCell* cell, int step);
    // sub_6B3E60: apply one step to the 2x2 grid block at (x, y), journalling
    // every write so a rejected step can be undone.
    bool ApplyElevationStep(int delta, int16_t x, int16_t y, unsigned int mask);
    // sub_6B3A80: validate a step, propagating the correction to the 8
    // neighbouring grid points (recursive).
    bool ValidateElevationStep(int delta, int16_t x, int16_t y);
    int  ElevationCornerAt(int index) const;
    bool ElevationIndexInRange(int index) const;
    void RollbackElevationJournal();

    // One journal entry: the grid point and the corner value it held. The
    // vanilla's dword_B0B654 buffer, 12 bytes per entry (x, y, old value).
    struct ElevationWrite
    {
        int index;
        int oldCorner;
    };
    std::vector<ElevationWrite> elevJournal_;

    bool IsPavedRoadEndTile(const MapCell* cell) const;
    bool IsMiscPaveTile(const MapCell* cell) const;
    bool IsPaveTile(const MapCell* cell) const;

    // Candidate-center search (sub_5A08D0). Picks a candidate center by
    // probability density; returns false when none qualifies.
    //   canyon lift (0x59e3a6): density 0.01, flag 1, rect {0,0,512,512},
    //                           anchor = the river's start cell - outputs
    //                           discarded
    //   delta fan   (0x59ec6a): density 0.003, flag 0 - outputs are the fan
    //                           center (doc §7 X_11 / v123)
    // The routine also mutates the work array: it stamps the generation code
    // on every absorbed cell (data[14]), sets the ring marker (byte 75) and
    // the visited marker (byte 15), and - when flag != 0 - walks the leftover
    // heap setting byte 56.
    bool FindCandidateCenter(int genCode, double density, const int* rect,
                             const CellStruct& anchor, int flag);

    // River rollback (sub_59D510 LABEL_151, 0x59e538 - 0x59e629): restore the
    // cells carrying `genCode` - and, when the river had placed a delta, those
    // carrying `genCode - 1` - to plain land at baseLevel_. Always returns
    // false so call sites read `return RollbackRiver(...)`.
    bool RollbackRiver(int genCode, int deltaCount);

    const MapSizeResult& GetSize() const { return size_; }

    // The radar base image the last ComputeRadarImage() produced, for the UI's
    // preview panel. Empty until a generation has run.
    const RadarImage& GetRadarImage() const { return radarImage_; }

    // ---- the progress ladder (sub_598960's sub_643C50 ticks) --------------
    // The vanilla advances its progress-bar object (dword_AC4F58) at fixed
    // points of the main flow: 55, 60, 65, 70, 75, 80, 85, 90, 95 percent and,
    // as the opening action of Done, 100 percent. On the single-player path
    // that is `sub_643C50(&progress, 0, percent, NaN)` - the network path pumps
    // the session instead (sub_69AE90 with the doubled argument 154..199). The
    // port takes the single-player semantics: ReportProgress forwards the
    // percentage to an optional sink, which the UI uses to draw the number.
    //
    // The values come from the main flow (today WinMain), because that is where
    // the vanilla emits them; see the ladder table in 5994B0_Done.c.
    typedef void (*ProgressSink)(void* context, int percent);

    // Installs / clears the sink. Done() clears it again, which is the port's
    // stand-in for sub_643E70 (that routine zeroes the progress object's hWnd).
    void SetProgressSink(ProgressSink sink, void* context);

    // One tick: clamps to 0..100, remembers the value and calls the sink.
    void ReportProgress(int percent);

    // The last value reported (0 before the first tick).
    int GetProgressPercent() const { return progressPercent_; }

    const RMGSettings& GetSettings() const { return settings_; }
    bool GetIniLoaded() const { return iniLoaded_; }

    // Cell slot array (512-stride). Diamond cells point into the pool,
    // everything else is nullptr (InvalidCell sentinel).
    MapCell* const* GetCellSlots() const { return cellSlots_; }
    int GetSlotRows() const { return slotRows_; }
    int GetCellCount() const { return cellCount_; }

    // [FA2sp] One cell by diamond coordinate, nullptr when outside the map.
    // cellSlots_ is indexed [row * 512 + x] with row == y.
    MapCell* GetCellAt(int x, int y) const
    {
        if (cellSlots_ == nullptr)
            return nullptr;
        if (x < 0 || x > 511 || y < 0 || y >= slotRows_)
            return nullptr;
        return cellSlots_[static_cast<size_t>(y) * 512 + static_cast<size_t>(x)];
    }

    // [FA2sp] Ledgers the .map writer used to read; the editor integration
    // consumes them straight into CMapData instead.
    const std::vector<MapStructure>& GetStructures() const { return structures_; }
    const std::vector<MapTerrainObject>& GetTerrainObjects() const { return terrainObjects_; }

    const WorkCell* GetWorkCells() const { return workCells_; }
    int GetWorkCellCount() const { return size_.workSide * size_.workSide; }

    bool GetRandom25Flag() const { return random25Flag_; }

    int GetWaterAmount() const { return waterAmount_; }

    // [FA2sp] Theater tile-family indices, exposed so the host can log them.
    // All -1 means LoadTheaterTiles found no INI, which makes every stage
    // stamp garbage tiles.
    int GetWaterTileIndexDebug() const { return waterTileIndex_; }
    int GetShorePiecesDebug() const { return shorePieces_; }
    int GetGreenTileDebug() const { return greenTileIndex_; }

    R250Random& RNG() { return rng_; }

    // Diamond existence test (MapClass::CreateEmptyMap):
    //   W' < x+y <= W'+2H'  and  |x-y| < W'
    bool CellExists(int x, int y) const
    {
        int s = x + y;
        if (s <= size_.mapWidth || s > size_.mapWidth + 2 * size_.mapHeight)
            return false;
        int d = x - y;
        if (d < 0) d = -d;
        return d < size_.mapWidth;
    }

private:
    MapSizeResult CalcMapSize(const MapGenConfig& cfg) const;
    void InitCells();
    void InitWorkArray();
    void FillWorkCoords();

    // ---- SmoothWaterBody (sub_57A0C0) support ----
    // Work array accessor by linear index (sub_58C2C0): base + 80 * index.
    WorkCell& WorkAtLinear(int index) { return workCells_[index]; }

    // Work array accessor by diamond coords (sub_58C2A0): base + 80*x + 80*W'*y.
    WorkCell& WorkAt(int x, int y) { return workCells_[x + size_.workSide * y]; }

    // ---- GenerateLake (sub_59C920) support ----
    // Ring-spread relabel (sub_5A0410). Called once by the lake auto-seed pass
    // as ClearPreviousGeneration(0, 2, -2) (0x59ca9a): grows `rings` rings
    // outward from the body carrying `genCode`, stamping each visited cell's
    // generation mark with `mark` and clearing its tile.
    void ClearPreviousGeneration(int genCode, int rings, int mark);

    // ---- DecorateWaterTiles (sub_59C630) support ----
    // Detail-tile stamping (sub_5A6C10). Stamps the isotile type `tileIndex`
    // and its whole image footprint anchored at `packedCoords`: per footprint
    // cell with an image frame, Height = col + row * CellsInX,
    // IsoTileTypeIndex = the type's ArrayIndex, SlopeIndex = frame[+0x2A],
    // Level = level + frame[+0x28] - 4 (only when level != -1), and the work
    // generation mark (byte 56) = genCode. The geometry comes from the family
    // footprint tables (CliffSet and the other cliff-like sets) plus the eight
    // water detail tiles (waterTileIndex_ + 0..7), which measure as full 2x2
    // rectangles with Z = 0; SlopeIndex (frame[+0x2A]) is not tabulated and is
    // written as 0 - see the definition.
    void PlaceWaterDetailTile(int tileIndex, int packedCoords, int genCode,
                              int level);

    // ---- Init regions (0x598C24) support - the stage subordinates ----
    // Declared and called by InitRegions; all of them are implemented:
    // ReleaseRegionObject (sub_5AC290), SeedRegionsFromWaterCells (sub_58CF90),
    // BuildRegionFromCell (sub_58C800, the shared "cell -> region record"
    // builder both seeders call), GrowRegionBody (sub_58E740),
    // CollectRegionBoundary (sub_58D410, the input list the two body passes
    // share), SweepRegionBody (sub_58E9B0) and AssignRemainingCells (sub_58D010).
    void ReleaseRegionObject(MapRegion* region, bool freeObject);  // sub_5AC290
    // sub_58BF70 - the region record constructor: operator new(0x50) with
    // id = dword_ABED14 (then ++), level = the anchor cell's Level, waterFamily =
    // sub_4865D0 of the anchor cell, cellCount / byte +26 / byte +0x24 = 0, empty
    // cell list and zeroed bounding rect, appended to the global region array.
    // Shared by BuildRegionFromCell (which then overwrites level / waterFamily)
    // and by SplitOrDropRegion.
    MapRegion* CreateRegionRecord(int packedCoords);
    void SeedRegionsFromWaterCells();                              // sub_58CF90
    MapRegion* BuildRegionFromCell(MapCell* cell);                 // sub_58C800
    void GrowRegionBody(MapRegion* region, int passes);            // sub_58E740
    // sub_58D410 - the region's boundary cells (the seed list of pass 0 of
    // GrowRegionBody and of SweepRegionBody): every cell carrying the region id
    // that has at least one in-diamond neighbour with a different region mark.
    std::vector<CellStruct> CollectRegionBoundary(MapRegion* region) const;
    void SweepRegionBody(MapRegion* region);                       // sub_58E9B0
    void AssignRemainingCells(unsigned char flag);                 // sub_58D010

    // ---- Making regions (0x598D44) support - the stage subordinates ----
    // Declared and called by MakeRegions. Every one of them is implemented:
    // RebuildRegionBodies (sub_58EBC0), its callee SplitOrDropRegion
    // (sub_58D620), the latter's priority helper RegionNodePriority
    // (sub_58C6F0), ReseedRegions (sub_58EF10), CliffPass (sub_579010),
    // LinkRegionNeighbours (sub_58F0C0), CarveRegionRamps (sub_5905D0),
    // FixCliffLevels (sub_5A19E0), PlaceCliffs (sub_578E60), CorrectCliffTiles
    // (sub_5A17F0) and FillGreenPlaceholders (sub_59B740). The decompiles are in
    // the project's decompile folder - see 598D44_MakingRegions.c.
    void RebuildRegionBodies();        // sub_58EBC0  rebuild + settle/merge
    // sub_58D620 - the merge pass's per-region callback: split / drop one large
    // region; returns true when the region was consumed (the caller then removes
    // and deletes it) and false when it must be kept. Vanilla returns 0 right
    // away when byte +26 is already set.
    bool SplitOrDropRegion(MapRegion* region);
    // sub_58C6F0 - the priority key of one growth node:
    //   theta = (dx == 0) ? pi/2 : TableAtan(-dy/dx) (+ pi when dx < 0);
    //   diff  = |theta - angle| wrapped into [0, pi];
    //   key   = Random() * kUnitScale * 2.5 + diff * 1.5 + dist * 0.15;
    // (dx, dy) = cell - seed and dist = sqrt(dx^2 + dy^2). Consumes ONE RNG draw.
    double RegionNodePriority(CellStruct seed, CellStruct cell, double angle);
    void ReseedRegions();              // sub_58EF10  re-seed + adjacency + ramps
    // sub_579010 - one recursive step of the "fill the depressions" pass
    // ReseedRegions runs over the whole diamond (§12.6.1 of the project notes).
    // Its only `return 0` sits behind `mode != -1`, so with the -1 the caller
    // passes it always answers 1.
    bool CliffPass(MapCell* cell, int mode);
    // sub_579B70 - the 8-bit height-direction mask of one cell: bit d is set when
    // the neighbour in direction d (Neighbours order) is exactly one Level higher
    // and is not a cliff-family tile. Bit layout: N 0x80, NE 0x01, E 0x02,
    // SE 0x04, S 0x08, SW 0x10, W 0x20, NW 0x40. Cells outside the diamond and
    // cells whose work byte +74 is 0 answer 0. Non-const: like the vanilla it
    // touches the shared InvalidCell sentinel through CellAt.
    int CliffMask(const MapCell* cell);
    // sub_4863D0 - the cliff-family tile test CliffMask needs: true when the
    // cell's tile is inside one of the cliff-like families (CliffSet 40 wide, the
    // four 4-wide waterfall families - with a per-family SlopeIndex test on their
    // first and fourth tile -, CliffRamps 20, WaterCaves 4, BridgeSet 16,
    // WoodBridgeSet 16, DestroyableCliffs 2 and WaterCliffs 28).
    bool IsCliffFamilyTile(const MapCell* cell) const;
    // sub_58F0C0 - build one region's neighbour-id vector (region +4).
    void LinkRegionNeighbours(MapRegion* region);
    // sub_5905D0 - carve the ramps between one region and its neighbours.
    void CarveRegionRamps(MapRegion* region);
    // sub_590970 - try to carve one ramp at `coords` towards the region
    // `targetId`; `owner` is the region the ramp is carved for (the vanilla keeps
    // it in ecx, so the builders receive it as their hidden `this`), `ratio` is
    // the caller's attempt counter * 0.01. Returns non-zero when a ramp was
    // carved.
    int CarveRampAt(MapRegion* owner, CellStruct coords, int targetId, float ratio);
    // sub_590FD0 - the ramp direction mask sub_590970 dispatches on, plus the
    // `ok` flag it starts at 1 and may clear. Called as
    // sub_590FD0(&coords, &flag, regionId, ratio).
    int RampMask(CellStruct coords, bool& ok, int regionId, double ratio);
    // sub_5A1E50 - true when the region `regionId` owns at least `threshold` cells
    // inside the w x h rectangle whose top-left cell is (x, y). The work marks are
    // counted row by row and the threshold is tested as the count grows, so the
    // scan stops early; cells outside the diamond are skipped.
    bool RectHasRegionCells(int x, int y, int w, int h, int regionId, int threshold);
    // sub_58D070 / sub_5A0090 - write the region-id mark (work[+56]) of one cell.
    void SetWorkMark(CellStruct coords, int value);
    // sub_594010 - the ramp builders' coverage precondition: all four corners of
    // the w x h rectangle at (x, y) must sit in the usable area, and every cell of
    // it must be marked with `owner->id` or `targetId` and carry a placeholder
    // tile (sub_486380). Any other cell answers false.
    bool RampRectClear(MapRegion* owner, int x, int y, int w, int h, int targetId);
    // F2I64(Random() * 2^-31) - the 0/1 draw sub_590970 jitters the ramp strips
    // with (each call is one RNG draw; the value is below 2 so the vanilla's
    // rejection loop never repeats).
    int DrawZeroOrOne();
    // The seven directed ramp builders sub_590970 picks between by mask pattern;
    // each takes the region the ramp is for (the vanilla's ecx), the two corner
    // coords of the strip it may carve and the region it ramps towards, and
    // answers non-zero when it placed the ramp. Named by their vanilla addresses
    // until their bodies are ported (the numbers are only ordinals):
    int RampBuilder1(MapRegion* owner, CellStruct from, CellStruct to, int targetId);   // sub_593AF0
    int RampBuilder2(MapRegion* owner, CellStruct from, CellStruct to, int targetId);   // sub_593550
    int RampBuilder3(MapRegion* owner, CellStruct from, CellStruct to, int targetId);   // sub_593030
    int RampBuilder4(MapRegion* owner, CellStruct from, CellStruct to, int targetId);   // sub_592440
    int RampBuilder5(MapRegion* owner, CellStruct from, CellStruct to, int targetId);   // sub_591D80
    int RampBuilder6(MapRegion* owner, CellStruct from, CellStruct to, int targetId);   // sub_591740
    int RampBuilder7(MapRegion* owner, CellStruct from, CellStruct to, int targetId);   // sub_5910F0
    // sub_5A7250 / sub_5A7440 / sub_5902C0 - the three rectangle predicates the
    // lateral-corridor builder (sub_58F2C0) gates its blocks with. All three
    // first run the diamond test on the four corners of {x, y, w, h} (the same
    // window CellExists tests: dword_ABED04 / dword_ABED08) and then walk the
    // rectangle row by row, rejecting the whole block on the first bad cell:
    //   TileRectClear       sub_5A7250 - a PavedRoads cell fails unless
    //                                     allowPavedRoads, a PavedRoadEnds cell
    //                                     unless allowPavedRoadEnds; otherwise
    //                                     the cell must be a placeholder (0 /
    //                                     0xFFFF) or a MiscPaveTile / PaveTile.
    //   TileRectLevelClear  sub_5A7440 - the same, plus every cell must carry
    //                                     the Level of the rectangle's first
    //                                     cell; both road families share the one
    //                                     flag.
    //   OverlayRectClear    sub_5902C0 - walks (w + 1) x (h + 1) cells and
    //                                     requires OverlayTypeIndex == -1, the
    //                                     first cell's Level, and a placeholder
    //                                     or water-family (sub_4865D0) tile.
    bool TileRectClear(int x, int y, int w, int h, bool allowPavedRoads,
                       bool allowPavedRoadEnds);
    bool TileRectLevelClear(int x, int y, int w, int h, bool allowRoads);
    bool OverlayRectClear(int x, int y, int w, int h);

    // sub_5904B0 - the bridge repair hut placer of sub_58F2C0. Scans the
    // (w + 1) x (h + 1) rectangle for a cell with OverlayTypeIndex == -1, a
    // placeholder tile and no object, and answers whether it found one. The
    // vanilla then puts a Neutral-owned "CABHUT" building there, so a found cell
    // also stops the caller's wider fallback rectangle from being searched.
    bool PlaceBridgeRepairHut(int x, int y, int w, int h);

    // The hut sub_5904B0 drops on the cell found above: the bridge repair hut.
    // The vanilla path (0x59054x - 0x5905b2):
    //   owner = HouseClass::FindByCountryIndex(
    //               HouseTypeClass::FindIndexOfName("Neutral"));
    //   index = sub_45E7B0("CABHUT");      // BuildingTypeClass::Array ID scan
    //   building = new BuildingClass(0x720 bytes);
    //   BuildingClass::CTOR(building, BuildingTypeClass::Array.Items[index],
    //                       owner);
    //   building->Unlimbo({ coords.X * 256 + 128,
    //                       coords.Y * 256 + 128, 0 }, DirType_North);
    // The port has no game object system, so route B is used instead: the hut is
    // appended to structures_ (see MapStructure) and the cell's AltFlags is
    // marked as occupied, which is the part of Unlimbo that the placer above
    // depends on.
    void CreateNeutralBridgeRepairHut(CellStruct coords);

    // The cross-section gate of sub_58F2C0's four probe walks: the two cells
    // (x, y) and (x + 2, y) (horizontal band) or (x, y) and (x, y + 2) (vertical
    // band) must both lie in the usable area (checkLevel 1) and neither may be a
    // cliff-family tile. A failing pair ends the walk.
    bool CorridorBandClear(int x, int y, bool horizontal);

    // sub_58F2C0 - the lateral-corridor (paved strip) builder the water-family
    // branch of CarveRegionRamps runs for every pair of same-Level regions that
    // touch the water region `water`: it picks a random cell of `water`, grows
    // a 3-wide corridor out of it until the two ends sit on regionA / regionB,
    // and stamps the corridor with overlays and paved-road tiles. The result is
    // discarded by the caller (vanilla's char return).
    void LinkSameLevelRegions(MapRegion* water, MapRegion* regionA, MapRegion* regionB);
    // The id -> record lookup the Making-regions helpers repeat inline: a linear
    // scan of regions_ comparing +8. Returns nullptr when no record carries the id
    // (vanilla reads one record past the array in that case).
    MapRegion* FindRegionById(int id) const;
    // MapClass::IsWithinUsableArea(cell, checkLevel) - the "playable diamond" test
    // that several ramp helpers gate on. It differs from CellExists in three ways:
    // the band is shifted by the cell's Level (+1 more when the cell carries a
    // SlopeIndex), it is bounded by VisibleRect instead of the whole map rect, and
    // it allows two extra cells at the far end. VisibleRect is the scenario's
    // [Map] LocalSize = {2, 5, W, H} (DisplayClass::LoadFromINI reads "LocalSize"
    // at 0x4ad760 and hands it to sub_654490 = MapClass::SetVisibleRect), so the
    // usable band is |x - y| < W and x + y within (level + W' + 10,
    // level + W' + 2H + 12].
    bool IsWithinUsableArea(CellStruct coords, bool checkLevel);
    // sub_578640 itself: the same band, but computed from an explicit rectangle
    // instead of the canonical LocalSize. sub_594870 calls it with the rect
    // inflated by (4, 4, -8, -8), which is a tighter band than the canonical
    // one. IsWithinUsableArea delegates here.
    bool IsWithinUsableRect(CellStruct coords, bool checkLevel,
                            int visX, int visY, int visW, int visH);
    void FixCliffLevels();             // sub_5A19E0  cliff edge/corner +-4 fix-up
    void PlaceCliffs();                // sub_578E60  cliff placement driver
    // sub_579620 - one cliff piece: resolves which CliffSet slot (the vanilla's
    // n34, 1..40) the cell's neighbourhood calls for and stamps it through
    // PlaceIsoTile. Answers false to stop the driver's walk.
    bool PlaceCliffPiece(MapCell* cell, int genCode);
    // Port-only pre-stamp guard: true when the CliffSet piece `slot` (1..40)
    // has a raised wall cell (z >= 4) riding a ramp BODY - 8-neighbour ramp
    // cells all >= 2 levels below with no same-level ramp-top seam.
    bool CliffPieceHitsRamp(const MapCell* anchor, int slot);
    // True when a proposed L8 cliff wall cell at (wx,wy) rides down a ramp
    // face (a low ramp neighbour exists but no ramp meets it at L7+).
    bool IsCliffWallRiding(int wx, int wy, int wallLevel);
    bool IsRampArtCell(const MapCell* n) const;
    // True only for the multi-cell SlopeSetPieces family (3x4/4x3 ramp faces)
    // whose art can reach a diagonally adjacent wall's screen space.
    bool IsBigRampPiece(const MapCell* n) const;
    // Re-stamp the surviving footprint cells of a ramp-rejected CliffSet
    // piece as smaller art (C8 vertical strip, C34 corner, C12-14 feet)
    // instead of leaving t0 flat ground. Returns the number of cells written;
    // 0 means every wall cell was rejected (no survivor) and the caller should
    // fall back to stamping the full piece, because a bare L4/L8 height gap
    // with no facade is worse than a minor ramp/wall z-fight.
    int TrimCliffPieceForRamp(const MapCell* anchor, int slot);
    void CorrectCliffTiles();          // sub_5A17F0  cliff correction pass
    // Port-only post pass (a Westwood-unfixed corner): replace CliffSet pieces
    // whose footprint cells were swallowed by CliffRamps / other CliffSet
    // pieces, and fill asymmetric foot rows of one-row wall pieces.
    void RepairCliffPieces();
    // sub_5A1350 - the corrected CliffSet tile for one cliff tile: maps the
    // family slot (tile - shoreTileIndex_) to the mate its neighbourhood calls
    // for, consuming one RNG draw on most slots.
    int ResolveCliffVariant(int tile);
    // Array.Items[tile]->CellsInX for a CliffSet tile - the family's column
    // count, tabulated as kCliffFootprints[slot].w (Height is encoded as
    // col + row * CellsInX, so this inverts it). Lives in MapGenRiver.cpp next
    // to that table.
    int CliffCellsInX(int tile) const;
    void FillGreenPlaceholders();      // sub_59B740  fill leftovers near green

    // ---- Recalculating cell attributes (0x598E1F) support ----
    // The stage walks the diamond and calls CellClass::RecalcAttributes
    // (sub_47D2B0) once per cell with cellLevel = -1, i.e. "recompute everything
    // except the level". RecalcAttributes looks the cell's overlay up in
    // OverlayTypeClass::Array, so the port loads its stand-in first - the same
    // thing OverlayTypeClass::LoadFromINI (0x5FE770) does for the engine - from
    // the rules INI ([OverlayTypes] plus the per-overlay Land / Tiberium /
    // NoUseTileLandType keys). See MapGenRecalc.cpp.
    void LoadOverlayTypes();

    // sub_56D3F0 - the shared index of the two parallel cell-attribute arrays
    // (LevelAndPassability and LevelAndPassabilityStruct2): x + y * (W' + H' + 1)
    // clamped into [0, count). That is exactly the index our work array uses
    // (workCells_ / WorkAt), so the tables line up with it one for one.
    int PassabilityIndex(int x, int y) const;

    // Loads every isotile's TMP image (the tiles under the project's tile-art
    // folder: <name><nn>.tem for theater 0, .sno for theater 1) and tabulates,
    // per tile, each cell's frame / terrain type / ramp type. Called once per
    // map, before the Recalculating-cell-attributes stage runs.
    void LoadTileCellAttrs(int theater);

    // The four helpers RecalcAttributes (sub_47D2B0) reaches. `cell` is the
    // cell's Height, i.e. its offset inside the tile - see TileCellAttr.
    //   sub_544C20 - the tile's `cell` carries an image frame
    //   sub_544BE0 - its LandType (TMP terrain type through dword_8288E4)
    //   sub_5471B0 - its ramp type
    //   sub_547150 - writes CellsInX and the Y adjustment the caller reads
    bool TileCellHasFrame(int tile, int cell) const;
    int  TileCellLandType(int tile, int cell) const;
    int  TileCellRampType(int tile, int cell) const;
    bool TileCellImageInfo(int tile, int cell, int* cellsInX, int* yAdjust) const;

    const TileCellAttr* TileCellAttrAt(int tile, int cell) const;

    // dword_8288E4 @ 0x8288E4 - the 16-entry TMP terrain type -> LandType table
    // sub_544BE0 reads. Values outside 0..15 answer LandType_Clear.
    static int LandTypeFromTerrainType(int terrainType);

    // Fills GroundType::Array from the rules INI's twelve floor sections.
    void LoadGroundTypes();
    const GroundTypeInfo& GroundTypeFor(int landType) const;

    // Fills TiberiumClass::Array from the rules INI's [Tiberiums] list.
    void LoadTiberiums();

    // ToTiberiumIdx (0x5FDD20) - which ore type the overlay belongs to, or -1
    // when it is not an ore at all. Note the vanilla answers 0 (not -1) for an
    // ore overlay that matches no table entry, and its caller only tests "-1",
    // so both outcomes mean "yes, this is ore" here too.
    int ToTiberiumIdx(int overlayIndex) const;

    // sub_483C80 - the cell's Passability, decided in this order:
    //   outside the usable area       -> OutsideMap
    //   overlay Crushable             -> Crushable
    //   overlay Wall                  -> Destroyable
    //   overlay floor Cost[2] == 0    -> Impassable
    //   overlay IsARock               -> Impassable
    //   overlay IsRubble              -> Passable
    //   LandType Water / Beach        -> Water / Beach
    //   floor Cost[2] <= 0.01         -> Impassable
    //   nothing on the cell           -> Passable
    // The vanilla then walks the cell's object list; the generator never puts an
    // object on a cell, so that walk always ends at Passable and is not modelled.
    void RecalcCellPassability(MapCell* cell);

    // CellClass::SetupLAT (0x47CA80). Rewrites cell->IsoTileTypeIndex when the
    // tile belongs to one of the LAT families, picking the family piece whose
    // four-neighbour pattern matches; see MapGenRecalc.cpp for the block list.
    void SetupLAT(MapCell* cell);

    // CellClass::RecalcAttributes (sub_47D2B0 @ 0x47D2B0, size 0xab4) - the one
    // call the "Recalculating cell attributes" stage makes per cell. `cellLevel`
    // is -1 at every call site in the generated map flow, meaning "do not change
    // the level".
    void RecalcAttributes(MapCell* cell, int cellLevel);

    // ---- FloodFill (sub_57A430) support ----
    // Tile predicates: sub_485060 (water tile, IsoTileTypeIndex in
    // [nIdx, nIdx+14)) and sub_486380 (placeholder tile, 0 or 0xFFFF).
    bool IsWaterTile(const MapCell* cell) const;
    bool IsPlaceholderTile(const MapCell* cell) const;

    // Cell accessor mirroring MapClass::GetCellAt_MapCrd: the cell at (x, y),
    // or invalidCell_ (MapCoords stamped, tile 0) when the slot is null or out
    // of range.
    MapCell* CellAt(int x, int y);

    // Raw slot accessor (Cells.Items[index]) - no InvalidCell fallback;
    // nullptr when the index is out of range or the slot is null.
    MapCell* RawSlot(int index);

    // 8-neighbour lookup (CellClass::GetNeighbourCell over the Neighbours
    // table): the cell one step from `cell` in direction `facing` (0 = N,
    // 1 = NE, ... 7 = NW), with the InvalidCell fallback.
    MapCell* GetNeighbourCell(const MapCell* cell, int facing);

    // TileNeighbourMask (sub_57B210): 8-direction connectivity mask of the
    // cell's water neighbours. mode 0 = require placeholder tile, mode 1 =
    // reject water tile, mode 2 = no tile gate. All modes return the same mask
    // when they do not early-return; a mask already cached in the work cell's
    // data[16] is returned as-is.
    int TileNeighbourMask(MapCell* cell, int mode);

    // Foundation-preview reset (sub_4A8BF0). Runs as the first statement of
    // SmoothWaterBody; clears the DisplayClass foundation state carried in
    // foundationPreview_ (see FoundationPreviewState).
    void ResetPreviewState();

    // Foundation cell stamping (sub_4A95A0, YRpp DisplayClass::MarkFoundation):
    // sets or clears AltCellFlags_ContainsBuilding on every cell of the
    // foundation list foundationPreview_.Data, each taken relative to baseCell.
    void MarkFoundation(CellStruct baseCell, bool mark);

    // The three smoothing passes driven by SmoothWaterBody:
    //   pass 1  FloodFill       (sub_57A430) - IMPLEMENTED; recursive flood
    //                                        fill, returns false on foreign-
    //                                        water pollution
    //   pass 2  CleanupTile     (sub_57A320) - IMPLEMENTED; placeholder tile
    //                                        reclaim (return ignored by caller)
    //   pass 3/4 SelectShoreTile(sub_57ACF0) - IMPLEMENTED (RNG draw, mask
    //                                        gate, n12 branch selection,
    //                                        mode-1 edge scans, plus the
    //                                        placement tail incl. the
    //                                        PlaceIsoTile stamp); consumes
    //                                        one RNG draw per cell.
    bool FloodFill(MapCell* cell, int genCode, int flag);
    int  CleanupTile(MapCell* cell, int genCode);
    bool SelectShoreTile(MapCell* cell, int mode, int genCode, int flag);

    // Foundation-preview center move (sub_4A91B0). Sets
    // foundationPreview_.CenterCell to `target` and returns the previous
    // center - the tail of SelectShoreTile uses it to point the isotile
    // foundation at the shore anchor cell. Vanilla has two branches; only
    // the CurrentFoundation_Data == null one is reachable here
    // (ResetPreviewState clears Data and nothing in the generation path ever
    // sets it again), which is exactly the swap below. The Data != null
    // branch (unmark the old footprint, mark the new one, recompute
    // InAdjacent / NoShrouded) and the "target == (0,0) -> read the mouse"
    // special case both need the live DisplayClass and never run.
    CellStruct SetFoundationCenter(CellStruct target);

    // Isotile foundation stamping (sub_57B440) - IMPLEMENTED, shared with the
    // cliff placement chain. Stamps the isotile currently selected in
    // currentBuildingType_ onto every cell of its foundation footprint,
    // anchored at foundationPreview_.CenterCell, subject to the vanilla
    // acceptance / occupancy rules.
    //   lo, hi    accepted existing-tile range (mode 1: 0..0, mode 2:
    //             shorePieces_..shorePieces_+41)
    //   level     the source cell's Level (vanilla reads a signed byte)
    //   genCode   generation code (forced to 0 when flag != 0)
    //   placed    out flag: cleared when the stamp is rejected
    //   flag      nonzero selects the "no generation code" variant
    // The caller (SelectShoreTile) ignores the return value and only reads
    // *placed, exactly as vanilla does.
    // Geometry source: the footprint is resolution-time data (the isotile type
    // object's ArrayIndex +0x294, foundation width +0x2E4 / height +0x2E8 and
    // the per-cell pointer array inside its SHP image at GetImage() + 0x10 -
    // all built by IsometricTileTypeClass::ReadINI from the theater's tile INI
    // and ART). The 42 shore footprints are therefore measured once from the
    // shipped tile art and tabulated in MapGen.cpp (see kShoreFootprints).
    bool PlaceIsoTile(int lo, int hi, int level, int genCode,
                      bool* placed, int flag);

    MapGenConfig config_;
    MapSizeResult size_;
    RMGSettings  settings_;
    bool         iniLoaded_;
    R250Random   rng_;

    // The engine's global Randomizer stand-in and the options it rolled. See
    // GlobalMapOptions; filled by RollGlobalOptions.
    R250Random      globalRng_;
    GlobalMapOptions globalOptions_;

    MapCell*  cellPool_;    // allocated CellClass objects (diamond cells)
    MapCell** cellSlots_;   // 512-stride pointer array with nullptr sentinel
    int       slotRows_;    // slot array height (W' + H' + 1)
    int       cellCount_;   // number of diamond cells

    WorkCell* workCells_;

    // ---- the hill stage's scratch elevation grid (sub_6B2A70 - sub_6B3850) ----
    // Built over the map's MapCoordBounds with Left / Top as the origin and
    // (Right + 1) as the row stride - the vanilla's dword_B0B6EC / Left / Top /
    // dword_8759A4. Freed by FinalizeElevationSlopes (= sub_6B3850's tail).
    std::vector<ElevationEntry> elevGrid_;
    int elevLeft_;
    int elevTop_;
    int elevStride_;

    // Per-tile "Morphable" flag from the theater INI. The INI's own wording is
    // "Can this tile set be modified using the raise/lower ground function?" -
    // exactly what the hill stage does, so it levels only morphable tiles.
    // Indexed by the absolute tile index; filled by LoadTheaterTiles.
    std::vector<uint8_t> tileMorphable_;

    // The "Compute Radar Image" stage's product; see RadarImage. Filled by
    // ComputeRadarImage and read by the UI to draw the preview panel.
    RadarImage radarImage_;

    // ---- progress ladder state (see the public block) ----
    ProgressSink progressSink_;      // the port's stand-in for dword_AC4F58's
    void*        progressContext_;   // callback slot (+4) / hWnd (+100)
    int          progressPercent_;   // the slot-0 percentage (0 = reset)

    // ---- region system (Init regions / Making regions) ----
    std::vector<MapRegion*> regions_;   // vanilla dword_ABDF94 + count dword_ABDFA0
    int regionIdCounter_;               // vanilla dword_ABED14: the next region id

    // MapClass::InvalidCell sentinel: returned by CellAt when the requested
    // slot is null / out of range. Only MapCoords is meaningful (stamped with
    // the requested coords); the tile fields stay at their constructed values.
    MapCell   invalidCell_;

    // DisplayClass foundation-preview state (see FoundationPreviewState).
    FoundationPreviewState foundationPreview_;

    bool random25Flag_;    // RMG this[196] (byte 0x310): "deltas allowed"
                           // switch, rolled once with the main RNG at the
                           // end of the land pass (sub_599650 0x59a4cc:
                           // Random_w() < 0.25). Gates the river-mouth
                           // delta trigger (sub_59D510 0x59dcfc).
    int  baseLevel_;        // RMG this[195] = 4
    int  usedWaterCells_;   // RMG this[193]: water cells already spent against
                            // the river / lake budget. Grown by the river step
                            // count on success (sub_59D510 0x59e525) and read
                            // by the lake budget (sub_59C920 0x59c970, doc
                            // §6.1). Reset per map alongside baseLevel_.
    int  genCode_;          // RMG this[194]: generation code, ++ per successful
                            // patch; generators enter with 1; reset to 0 at the
                            // end of sub_598960 (0x5993b5)
    int  waterAmount_;      // RMG this[19]: water amount percent (0-100).
                            // Set from MapGenConfig::waterAmount, which the
                            // UI layer rolls per generation (sub_597260 @
                            // 0x597282: RandomRanged(0x82B0A8[landType],
                            // 0x82B0BC[landType]) with the UI randomizer
                            // this_pRandomizer - a different stream from
                            // dst_, so this roll never consumes generation
                            // RNG). A roll of 0 keeps LandType 3/4 all-land
                            // (sub_59C580 skipped, same as vanilla).
    // ---- 输出目录 -----------------------------------------------------------
    // 成品地图（.map/.yrm）和雷达图 radar_preview.png 的落盘位置，保存为带
    // 结尾反斜杠的绝对路径。构造时默认 = exe 所在文件夹，SetOutputDir 可改。
    std::wstring outputDir_;
    // DiagLog 是静态方法，访问不到 this->outputDir_，目录在这里另存一份静态
    // 拷贝，SetOutputDir 时同步。进程里还没调用过 SetOutputDir 时它为空，
    // DiagLog 就回退到 exe 所在文件夹。
    static std::wstring s_diagDir_;

    // ---- theater tile-family indices (IsometricTileTypeClass::ReadINI) ----
    // All of these come from [General] keys of the theater INI; LoadTheaterTiles
    // turns each key (a TileSet SECTION NUMBER) into the running tile count at
    // that section - see its comment in MapGenRiver.cpp:
    //   waterTileIndex_          <- WaterSet           nIdx   @0xAA0738
    //   shoreTileIndex_          <- CliffSet           @0xAA1020 (40 wide)
    //   greenTileIndex_          <- GreenTile          @0xAA0E18
    //   clearToGreenLatIndex_    <- ClearToGreenLat    @0xAA0748 (16 wide)
    //   waterCliffsIndex_        <- WaterCliffs        (Cliff/Water pieces)
    //   destroyableCliffsIndex_  <- DestroyableCliffs
    //   shorePieces_             <- ShorePieces        nIdx_4 @0xABAD28
    //   waterFamily4Base_[0..3]  <- WaterfallEast / West / South / North
    int  waterTileIndex_;          // WaterSet: base tile of the water group
    int  shoreTileIndex_;          // CliffSet: base of the cliff family (40 wide)
    int  greenTileIndex_;          // game global dword_AA0E18 (IDB name
                                   // IsoTileTypeIndex_1): base of the GreenTile
                                   // ground set - the tile sub_59D510 (0x59e301)
                                   // and sub_59C920 (0x59d395) stamp on the cells
                                   // still sitting on 0 / 0xFFFF, and the tile
                                   // sub_59B740 fills the leftover placeholders
                                   // next to green ground with
    int  clearToGreenLatIndex_;    // game global dword_AA0748: base of the 16-wide
                                   // ClearToGreenLat transition set
                                   // (sub_4867B0's second branch)
    int  waterCliffsIndex_;        // WaterCliffs: Cliff/Water pieces (28 wide)
    int  destroyableCliffsIndex_;  // DestroyableCliffs (2 wide)
    int  cliffRampsIndex_;         // CliffRamps (20 wide)
    int  waterCavesIndex_;         // WaterCaves (4 wide)
    int  bridgeSetIndex_;          // BridgeSet (16 wide)
    int  woodBridgeSetIndex_;      // WoodBridgeSet (16 wide)
    int  waterFamily4Base_[4];     // the four 4-wide waterfall families, in
                                   // sub_4865D0's test order (East, West, South,
                                   // North)
    int  rampBaseIndex_;           // game global IsoTileTypeIndex_2 @0xABC1D8:
                                   // the [General] key "RampBase" family base.
                                   // sub_593AF0 stamps rampBaseIndex_ + 0 / + 3 /
                                   // + 7 for the three ramp bands.
                                   // The theater INI resolves it to the tile set
                                   // whose SetName is "Ramps" and whose FileName is
                                   // "slope": RampBase = 9 -> [TileSet0009] slope,
                                   // TilesInSet 20 (temperat.ini / temperatmd.ini),
                                   // so the three bands stay inside the family.
    int  slopeSetPiecesIndex_;     // game global dword_ABC1F8: the [General] key
                                   // "SlopeSetPieces" family base. sub_593AF0 puts
                                   // its 2 x 2 pieces at + 1 and + 7 through
                                   // sub_5A6C10.
                                   // SlopeSetPieces = 25 -> [TileSet0025], SetName
                                   // "Slope Set Pieces", FileName "RAMP",
                                   // TilesInSet 10 (= the rmpx01..10 art).
                                   // NOTE the crossed naming: the family named
                                   // "Ramps" is file-named "slope" and the family
                                   // named "Slope Set Pieces" is file-named "RAMP".
    int  pavedRoadsIndex_;         // game global dword_ABBEC8: base of the
                                   // [General] key "PavedRoads" family (15 wide,
                                   // sub_4866D0). sub_58F2C0 stamps + 9 / + 10 /
                                   // + 12 / + 13 through sub_5A6C10.
    int  pavedRoadEndsIndex_;      // game global dword_ABBEC4: base of the
                                   // [General] key "PavedRoadEnds" family
                                   // (4 wide, sub_4866F0). sub_58F2C0 stamps
                                   // + 0 / + 1 / + 2 / + 3.
    int  miscPaveTileIndex_;       // game global dword_AA10A4: base of the
                                   // [General] key "MiscPaveTile" family
                                   // (14 wide, sub_486650) - accepted by
                                   // sub_5A7250 / sub_5A7440 as a valid cell.
    int  paveTileIndex_;           // game global dword_ABC2B0: base of the
                                   // [General] key "PaveTile" family
                                   // (16 wide, sub_486670) - accepted by
                                   // sub_5A7250 / sub_5A7440 as a valid cell.

    // ---- theater shore-tile data (IsometricTileTypeClass::ReadINI) ----
    int        shorePieces_;          // game global dword_ABAD28: base index
                                      // of the shore tile group (the [General]
                                      // key "ShorePieces"); valid shore
                                      // variants are shorePieces_ + (0..41).
    CellStruct shoreAnchor_[42];      // game global table word_ABDB64: paired
                                      // int16 X/Y anchor offsets indexed by
                                      // the shore variant n12. All {0,0} in
                                      // the static image; filled at runtime.
    // ---- Recalculating cell attributes (0x598E1F) ----
    // The overlay type table indexed by OverlayTypeIndex - the port's stand-in
    // for OverlayTypeClass::Array. Filled by LoadOverlayTypes (MapGenRecalc.cpp).
    std::vector<OverlayTypeInfo> overlayTypes_;

    // The two parallel cell-attribute arrays MouseClass::Instance keeps, both
    // addressed through PassabilityIndex (sub_56D3F0). Sized like the work
    // array; the second one records the level only.
    std::vector<CellLevelPassability> levelAndPassability_;
    std::vector<int>                  levelAndPassability2_;

    // Per-tile TMP cell attributes, indexed by tile index; an empty entry means
    // the loader found no art for that tile. Filled by LoadTileCellAttrs.
    std::vector<std::vector<TileCellAttr> > tileCellAttrs_;

    // GroundType::Array - indexed by LandType. Filled by LoadGroundTypes.
    std::vector<GroundTypeInfo> groundTypes_;

    // RulesClass::Instance->CliffBackImpassability (rules INI [General]). The
    // shipped value is 2; only 2 rewrites a floor to LandType_Rock.
    int cliffBackImpassability_;

    // The buildings the generator places, in the order it places them. See
    // MapStructure: this is the "B route" stand-in for a live object system, and
    // the source the .map [Structures] section will read.
    std::vector<MapStructure> structures_;

    // The terrain objects the generator places (so far only the ore decoration
    // trees of sub_5A28C0) - the source the .map [Terrain] section will read.
    // See MapTerrainObject.
    std::vector<MapTerrainObject> terrainObjects_;

    // ---- the starting-point ledger (route B for ScenarioClass) -------------
    // The vanilla writes three places; the port records the same information
    // here instead:
    //   waypoints_          ScenarioClass::Waypoints (+0x632): the packed coords
    //                       per waypoint index, 702 entries. Written by
    //                       StampStartingPointCells with the same store
    //                       sub_68BF50 does, read back the way
    //                       ScenarioClass::GetWaypointCoords does.
    //   startingPoints_     the ledger the .map writer reads - see
    //                       StartingPointRecord.
    //   startingPointCount_ how many starting points were stamped, i.e. the
    //                       running offset sub_594B50 ends on. sub_5A1FB0 loops
    //                       over this many waypoints (vanilla reads the count
    //                       from MapClass+0x50, which the port has no field for).
    // Deliberately not modelled: sub_58B820, the publish step sub_594B50 ends on
    // (the only call site is 0x594EE3). It fills the instance's starting-point
    // tables - dword_ABE304 / dword_ABE328 / dword_ABE32C = this[203] / [212] /
    // [213] - and the SCENARIO's NumberStartingPoints / waypoint arrays /
    // StartX / StartY / Width / Height, taking the coords from the waypoints and
    // the extents from a whole-map scan of the usable area's pixel box
    // (sub_6D62E0). None of it is cell data, and every reader of those fields is
    // either the RMG dialog (sub_596300) or a UI helper (sub_58BB00 - reached
    // only from sub_5E7EB0 - and sub_58BB30, reached from sub_5E66F0 /
    // sub_5E74E0 / sub_5ED5A0 / sub_6AE6E0); no RMG stage reads them, so the
    // port keeps its own waypoints_ / startingPoints_ ledger instead.
    // (dword_ABE300 is 0: sub_58B6D0 is its only writer and stores 0.)
    std::vector<CellStruct>          waypoints_;
    std::vector<StartingPointRecord> startingPoints_;
    int                              startingPointCount_;

    // RulesClass::NeutralTechBuildings - the tech buildings sub_595400 may drop
    // on a region that came out with a zero share. Filled by
    // LoadNeutralTechBuildings (see NeutralTechBuilding).
    std::vector<NeutralTechBuilding> neutralTechBuildings_;

    // Rules INI [Countries] in index order, with each country's Color - the
    // [Houses] / [<country>] pairs the multiplayer .yrm layout writes. Filled by
    // LoadMultiplayerHouses (see MultiplayerHouse).
    std::vector<MultiplayerHouse> multiplayerHouses_;

    // MapClass::MovementZones - thirteen cost tables (YRpp declares
    // `void* MovementZones[13]`), indexed by a cell's ZoneArrayIndex. Rebuilt by
    // RebuildMoveZones (sub_56C510). sub_594B50 only ever reaches the five it
    // picks from (3 x Amphibious, 2 x Normal).
    std::vector<int> movementZones_[13];

    // The thirteen passability tables sub_56C510 builds at its tail: for each
    // CellPassability value, whether the zone is walkable. Static source data.
    std::vector<uint8_t> zonePassability_[13];

    // MapClass::ZoneConnections - the table sub_56CB90 records "these two zones
    // touch" pairs into. The key is (zone & 0xF) | (otherZone << 4), so the
    // table has 256 entries; each holds the partners recorded for that key.
    // Vanilla's entry is a VectorClass<unsigned long> that stores every key
    // twice; the port keeps one copy, since the only uses are "already there?"
    // and "append".
    std::vector<int> zoneConnections_[256];

    // sub_56CB90's scratch "last partner recorded" global (dword_ABDE8C).
    int lastRecordedZone_;

    // The per-cell passability the zone rebuild reads BEFORE it floods over
    // levelAndPassability_ (vanilla's VectorClass<PassabilityType>). The walk
    // overwrites CellPassability with the zone number, so the original values
    // are kept here for the thirteen passability tables built at the tail.
    std::vector<uint8_t> passabilityCopy_;

    // Per zone: the passability its seed cell had. The thirteen tables at the
    // tail look this up as `kZonePassability[type][zonePassability_value]`.
    std::vector<uint8_t> zonePassabilityValue_;

    // TiberiumClass::Array. Filled by LoadTiberiums.
    std::vector<TiberiumInfo> tiberiums_;

    // ---- the rest of the theater [General] tile-family bases ----
    // LoadTheaterTiles reads every one of these; SetupLAT (sub_47CA80) uses the
    // subset it needs. Names follow the INI keys, and the comments give the game
    // global each one mirrors.
    int rampSmoothIndex_;         // RampSmooth        0xAA1058
    int mmRampBaseIndex_;         // MMRampBase        0xAA109C
    int clearTileIndex_;          // ClearTile         0xAA10B0
    int roughTileIndex_;          // RoughTile         0xABC2B8
    int sandTileIndex_;           // SandTile          0xABB104
    int clearToRoughLatIndex_;    // ClearToRoughLat   0xAA1134
    int clearToSandLatIndex_;     // ClearToSandLat    0xAA0E24
    int clearToPaveLatIndex_;     // ClearToPaveLat    0xAA10A8
    int heightBaseIndex_;         // HeightBase        0xAA0744
    int blackTileIndex_;          // BlackTile         0xABC2CC
    int slopeSetPieces2Index_;    // SlopeSetPieces2   0xAA1098
    int monorailSlopesIndex_;     // MonorailSlopes    0xAA1024
    int tunnelsIndex_;            // Tunnels           0xAA1054
    int trackTunnelsIndex_;       // TrackTunnels      0xABB108
    int dirtTunnelsIndex_;        // DirtTunnels       0xAA10B4
    int dirtTrackTunnelsIndex_;   // DirtTrackTunnels  0xABAD2C
    int mediansIndex_;            // Medians           0xAA0E20
    int roughGroundIndex_;        // RoughGround       0xAA0E1C
    int dirtRoadJunctionIndex_;   // DirtRoadJunction  0xABAD20
    int dirtRoadCurveIndex_;      // DirtRoadCurve     0xABC1D4
    int dirtRoadStraightIndex_;   // DirtRoadStraight  0xAA10AC
    int pavedRoadSlopesIndex_;    // PavedRoadSlopes   0xAA1094
    int dirtRoadSlopesIndex_;     // DirtRoadSlopes    0xABBEC0
    int rocksIndex_;              // Rocks             0xABB10C
    int waterBridgeIndex_;        // WaterBridge       0xAA1090

    int        currentBuildingType_;  // MouseClass::Instance.CurrentBuildingType:
                                      // the isotile index currently selected
                                      // for placement - set by SelectShoreTile,
                                      // consumed by PlaceIsoTile.

    // Per-footprint-cell results of the PlaceCliffs CliffSet stamps, consumed
    // once by RepairCliffPieces after CorrectCliffTiles.
    std::vector<CliffStampOutcome> cliffStamps_;
};

// sub_4CADE0 - the game's table-based atan. Uses the exact flt_8610B4 float
// bits (MapGenAtanTable.h); std::atan differs by 1-14 float ULP and desyncs the
// region-growth and river-shore geometry.
double TableAtan(double x);

