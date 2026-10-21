#include "pch.h"
#include "MapGen.h"
#include "MapGenFastSqrt.h"

#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <cwchar>
#include <cstdio>
#include <windows.h>
#include <strsafe.h>

// ============================================================================
// [TEMP DIAG] DiagLog - append one ASCII line to <输出目录>\rmg_diag.log.
// 输出目录跟随界面选择（默认 exe 所在文件夹）；进程里还没生成过对象时静态
// 目录为空，回退到 exe 所在文件夹。First call in the process truncates the
// file; GenerateMapBody also prints a banner so several GUI runs in one
// process stay separable.
// ============================================================================
void RandomMapGenerator::DiagLog(const char* fmt, ...)
{
    if (!s_bDiagLogEnabled)
        return;

    // 先取 exe 所在文件夹，作为静态目录为空时的回退。
    wchar_t exeDir[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH) == 0)
        return;
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash == nullptr)
        return;
    slash[1] = L'\0';

    const std::wstring dir =
        s_diagDir_.empty() ? std::wstring(exeDir) : s_diagDir_;
    CreateDirectoryW(dir.c_str(), nullptr);   // 已存在时失败，忽略

    wchar_t path[MAX_PATH] = {};
    if (dir.size() >= MAX_PATH - 16)
        return;
    wcscpy_s(path, MAX_PATH, dir.c_str());
    wcscat_s(path, MAX_PATH, L"rmg_diag.log");

    static bool first = true;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, first ? L"w" : L"a") != 0 || fp == nullptr)
        return;
    first = false;

    char buf[600];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fputs(buf, fp);
    fputc('\n', fp);
    fclose(fp);
}

// ============================================================================
// Size interpolation tables (players 2-8, index 0-6)
// Source: 0x7ED634 / 0x7ED650 / 0x7ED66C / 0x7ED688
// ============================================================================
static const int kWmin[7] = { 70, 70, 70, 80, 90, 100, 100 };
static const int kWmax[7] = { 80, 80, 80, 90, 100, 110, 120 };
static const int kHmin[7] = { 70, 70, 70, 80, 90, 100, 100 };
static const int kHmax[7] = { 80, 80, 80, 90, 100, 110, 120 };

static const float kSizeClamp = 1.2f;
static const float kOneThird  = 1.0f / 3.0f;

// F2I64 (Game::F2I64 0x7C5F00, truncate toward zero) moved to MapGen.h - both
// this file and MapGenRiver.cpp use it.

// ============================================================================
// R250 Random Number Generator - exact replication of dst_ (0xABE890)
// ============================================================================

// Seed mixing tables (0x839644 / 0x839690, little-endian dwords)
static const uint32_t kSeedT1[4] =
    { 0xBAA96887u, 0x1E17D32Cu, 0x03BCDC3Cu, 0x0F33D1B2u };
static const uint32_t kSeedT2[5] =
    { 0x48AAD7E4u, 0x4B0F3B58u, 0xE874F0C3u, 0x6955C5A6u, 0x55A7CA46u };

// kUnitScale (Random_w scale, dbl_7ED898) moved to MapGen.h - both this file
// and MapGenRiver.cpp use it.

R250Random::R250Random()
    : index1_(0), index2_(103), gaussHave_(false), gaussSaved_(0.0)
{
    for (int i = 0; i < 250; ++i)
        buffer_[i] = 0;
}

// ---------------------------------------------------------------------------
// Seed (sub_65C6D0)
//
// Per-slot 4-round mixing. Assembly-verified details:
//   - v3 resets to the seed constant for every slot
//   - v4 chain: v4(slot) -> v8 -> v8 -> v8, written to buffer[slot]
//   - v3 chain: seed -> slot -> v8_1 -> v8_2 (previous v4 each round)
//   - hi = v7 SAR 16 (arithmetic), lo = v7 & 0xFFFF
//   - t = lo*lo + ~(hi*hi)   (imul/mul low 32 bits are identical)
//   - rot = (t SAR 16) | (t SHL 16)  - NOT a pure rotate when t < 0
//   - v8 = v3 ^ (hi*lo + (T2[n+1] ^ rot)), T2 indexed 1..4
// ---------------------------------------------------------------------------
void R250Random::Seed(uint32_t seed)
{
    for (int slot = 0; slot < 250; ++slot)
    {
        uint32_t v3 = seed;
        uint32_t v4 = (uint32_t)slot;
        for (int n = 0; n < 4; ++n)
        {
            uint32_t prev = v4;
            uint32_t v7 = v4 ^ kSeedT1[n];
            uint32_t hi = (uint32_t)((int32_t)v7 >> 16);  // sar
            uint32_t lo = v7 & 0xFFFFu;
            uint32_t hihi = hi * hi;
            uint32_t hilo = hi * lo;
            uint32_t lolo = lo * lo;
            uint32_t t = lolo + ~hihi;
            uint32_t rot = ((uint32_t)((int32_t)t >> 16)) | (t << 16);
            uint32_t v8 = v3 ^ (hilo + (kSeedT2[n + 1] ^ rot));
            v4 = v8;
            v3 = prev;
        }
        buffer_[slot] = v4;
    }
    index1_ = 0;    // this[1]
    index2_ = 103;  // this[2]
    // sub_598960 also resets the GaussState at 0xABDFB8 right after
    // seeding dst_: {flag byte = 0, saved = 0.0, fn = Random_w}
    gaussHave_ = false;
    gaussSaved_ = 0.0;
}

// ---------------------------------------------------------------------------
// Next (Randomizer::Random, 0x65C780)
//   buffer[index1] ^= buffer[index2]; result = buffer[index1];
//   both indices advance with wrap at 250.
//   (byte-size guard at this[0] is permanently 0 after seeding - skipped)
// ---------------------------------------------------------------------------
uint32_t R250Random::Next()
{
    buffer_[index1_] ^= buffer_[index2_];
    uint32_t result = buffer_[index1_];

    int i1 = index1_ + 1;
    int i2 = index2_ + 1;
    index1_ = i1;
    index2_ = i2;
    if (i1 >= 250)
        index1_ = 0;
    if (i2 >= 250)
        index2_ = 0;

    return result;
}

// ---------------------------------------------------------------------------
// RandomRanged (Randomizer::RandomRanged, 0x65C7E0)
//   - min == max -> min; else swap so min < max
//   - bit scan: n31 walks 30..0 (pre-decrement in asm), n31 = msb(nRange)
//   - mask = low (n31+1) bits; rejection: r = mask & Next() while r > range
//   - consumes a variable number of Next() calls
// ---------------------------------------------------------------------------
int R250Random::RandomRanged(int min, int max)
{
    if (min == max)
        return min;

    int nMin = min;
    int nMax = max;
    if (min > max)
    {
        nMin = max;
        nMax = min;
    }

    int nRange = nMax - nMin;

    int n31 = 31;
    if (nRange >= 0)
    {
        do
        {
            if (n31 <= 0)
                break;
            --n31;
        } while (((1 << n31) & nRange) == 0);
    }

    int mask = ~(-1 << (n31 + 1));
    int r = nRange + 1;
    while (r > nRange)
        r = (int)((uint32_t)mask & Next());

    return nMin + r;
}

// ---------------------------------------------------------------------------
// RandomFloatRange (sub_598030, fastcall: ecx=lo, edx=hi)
//   range = (double)(uint32)(hi - lo + 1)     [fild via zero-extended qword]
//   base  = (double)(uint32)lo
//   loop: F2I64(Next() * range * kUnitScale + base); reject if > hi (unsigned)
// ---------------------------------------------------------------------------
int R250Random::RandomFloatRange(int lo, int hi)
{
    double range = (double)(uint32_t)((uint32_t)hi - (uint32_t)lo + 1u);
    double base = (double)(uint32_t)lo;

    for (;;)
    {
        uint32_t r = Next();
        double x = (double)(uint64_t)r * range * kUnitScale + base;
        int64_t v = (int64_t)x;  // F2I64: truncate toward zero
        if ((uint32_t)v <= (uint32_t)hi)  // asm: cmp eax, esi / ja
            return (int)v;
    }
}

// ---------------------------------------------------------------------------
// Unit (Randomizer::Random_w, 0x598000)
//   fild(zero-extended Next()) then fmul dbl_7ED898
// ---------------------------------------------------------------------------
double R250Random::Unit()
{
    return (double)(uint64_t)Next() * kUnitScale;
}

// ---------------------------------------------------------------------------
// Chance25 (sub_599650 @ 0x59a4cc inline)
//   flag = Random() * kUnitScale < 0.25
//   (integer-equivalent: Next() <= 0x3FFFFFFF; no boundary ambiguity)
// ---------------------------------------------------------------------------
bool R250Random::Chance25()
{
    return Next() * kUnitScale < 0.25;
}

// ---------------------------------------------------------------------------
// Gaussian (sub_5980C0, Marsaglia polar method)
//
// Game state (GaussState at 0xABDFB8, reset in sub_598960):
//   +0x00 flag byte, +0x08 saved double, +0x10 fn ptr -> Random_w
// The uniform samples come from the SAME main RNG dst_ (via Random_w),
// each Unit() call consumes exactly one Next().
//
// asm-verified order: first call -> u, second call -> v;
// s = v*v + u*u; reject s >= 1.0, then reject s == 0.0;
// factor = sqrt((-ln(s) - ln(s)) / s); saved = factor*v, return factor*u.
// ---------------------------------------------------------------------------
double R250Random::Gaussian()
{
    if (gaussHave_)
    {
        gaussHave_ = false;
        return gaussSaved_;
    }

    double a = Unit();
    double b = Unit();
    double u = a + a - 1.0;
    double v = b + b - 1.0;
    double s = v * v + u * u;
    while (s >= 1.0 || s == 0.0)
    {
        a = Unit();
        b = Unit();
        u = a + a - 1.0;
        v = b + b - 1.0;
        s = v * v + u * u;
    }

    double lns = std::log(s);  // asm: fldln2 + fyl2x
    double factor = std::sqrt((-lns - lns) / s);

    gaussHave_ = true;
    gaussSaved_ = factor * v;
    return factor * u;
}

// ============================================================================
// RandomMapGenerator implementation
// ============================================================================

RandomMapGenerator::RandomMapGenerator()
    : iniLoaded_(false)
    , cellPool_(nullptr)
    , cellSlots_(nullptr)
    , slotRows_(0)
    , slotStride_(512)
    , cellCount_(0)
    , workCells_(nullptr)
    , random25Flag_(false)
    , baseLevel_(4)
    , usedWaterCells_(0)
    , genCode_(0)
    , waterTileIndex_(-1)
    , shoreTileIndex_(-1)
    , greenTileIndex_(-1)
    , clearToGreenLatIndex_(-1)
    , waterCliffsIndex_(-1)
    , destroyableCliffsIndex_(-1)
    , cliffRampsIndex_(-1)
    , waterCavesIndex_(-1)
    , bridgeSetIndex_(-1)
    , woodBridgeSetIndex_(-1)
    , rampBaseIndex_(-1)
    , slopeSetPiecesIndex_(-1)
    , pavedRoadsIndex_(-1)
    , pavedRoadEndsIndex_(-1)
    , miscPaveTileIndex_(-1)
    , paveTileIndex_(-1)
    , waterAmount_(0)
    , shorePieces_(-1)
    , rampSmoothIndex_(-1)
    , mmRampBaseIndex_(-1)
    , clearTileIndex_(-1)
    , roughTileIndex_(-1)
    , sandTileIndex_(-1)
    , clearToRoughLatIndex_(-1)
    , clearToSandLatIndex_(-1)
    , clearToPaveLatIndex_(-1)
    , heightBaseIndex_(-1)
    , blackTileIndex_(-1)
    , slopeSetPieces2Index_(-1)
    , monorailSlopesIndex_(-1)
    , tunnelsIndex_(-1)
    , trackTunnelsIndex_(-1)
    , dirtTunnelsIndex_(-1)
    , dirtTrackTunnelsIndex_(-1)
    , mediansIndex_(-1)
    , roughGroundIndex_(-1)
    , dirtRoadJunctionIndex_(-1)
    , dirtRoadCurveIndex_(-1)
    , dirtRoadStraightIndex_(-1)
    , pavedRoadSlopesIndex_(-1)
    , dirtRoadSlopesIndex_(-1)
    , rocksIndex_(-1)
    , waterBridgeIndex_(-1)
    , cliffBackImpassability_(-1)
    , currentBuildingType_(0)
    , regionIdCounter_(0)
{
    std::memset(&config_, 0, sizeof(config_));
    std::memset(&size_, 0, sizeof(size_));
    std::memset(shoreAnchor_, 0, sizeof(shoreAnchor_));
    // ReadINI leaves every tile-family global at -1 before filling it.
    for (int i = 0; i < 4; ++i)
        waterFamily4Base_[i] = -1;
    settings_.SetDefaults();

    // The progress ladder starts idle: no sink (Done clears it again) and the
    // slot-0 percentage at 0, the state sub_643C50(0, 0.0) leaves behind.
    progressSink_ = nullptr;
    progressContext_ = nullptr;
    progressPercent_ = 0;

    // 默认输出目录 = exe 所在文件夹（带结尾反斜杠）。界面不改的话，地图、
    // 雷达图和诊断日志就直接落在 exe 旁边。
    {
        wchar_t modPath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, modPath, MAX_PATH) != 0)
        {
            wchar_t* slash = wcsrchr(modPath, L'\\');
            if (slash != nullptr)
            {
                slash[1] = L'\0';
                outputDir_ = modPath;
                s_diagDir_ = outputDir_;
            }
        }
    }
}

// 静态成员的实体定义（声明在 MapGen.h）。
std::wstring RandomMapGenerator::s_diagDir_;

// [FA2sp] Diagnostic logging is OFF by default - see MapGen.h.
bool RandomMapGenerator::s_bDiagLogEnabled = false;

// [FA2sp] RMGMD.INI is not read unless the standalone tool asks for it.
bool RandomMapGenerator::s_bLoadRmgmdIni = false;

// [FA2sp] Empty = look for the game INIs next to the executable.
std::wstring RandomMapGenerator::s_gameIniDir;

// [FA2sp] Single place that decides which copy of rulesmd.ini / artmd.ini /
// temperatmd.ini the generator reads. Under the editor extension the host sets
// s_gameIniDir to the game folder, which is exactly where FA2 loaded them from.
bool RandomMapGenerator::ResolveGameIni(const wchar_t* fileName, wchar_t* out,
                                        int outSize)
{
    if (out == nullptr || outSize <= 0)
        return false;

    // [FA2sp] With a host INI source there is no file to locate: hand the
    // logical name back so the read shims know which INI the caller wants.
    if (s_iniSource != nullptr)
    {
        wcsncpy_s(out, static_cast<size_t>(outSize), fileName, _TRUNCATE);
        return true;
    }

    wchar_t dir[MAX_PATH];
    if (!s_gameIniDir.empty())
    {
        wcsncpy_s(dir, MAX_PATH, s_gameIniDir.c_str(), _TRUNCATE);
    }
    else
    {
        if (GetModuleFileNameW(nullptr, dir, MAX_PATH) == 0)
            return false;
        if (wchar_t* slash = wcsrchr(dir, L'\\'))
            slash[1] = L'\0';
    }

    // "\u76f8\u5173INI" is the rules-INI folder's name, escaped so this source
    // file stays pure ASCII.
    static const wchar_t* const kCandidates[] =
    {
        L"%s%s",                                    // the folder itself
        L"%s..\\%s",
        L"%s..\\..\\\u76f8\u5173INI\\%s",
        L"%s\u76f8\u5173INI\\%s",
    };

    for (int i = 0; i < 4; ++i)
    {
        wchar_t path[MAX_PATH];
        if (swprintf_s(path, kCandidates[i], dir, fileName) < 0)
            continue;
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        {
            wcsncpy_s(out, static_cast<size_t>(outSize), path, _TRUNCATE);
            return true;
        }
    }
    return false;
}

// [FA2sp] When true, the six Load* passes are skipped and the host's tables
// (pushed through SetTileCellAttrs / SetOverlayTypes / ...) are used instead.
bool RandomMapGenerator::s_bUseHostData = false;

// [FA2sp] Non-null once the host supplies its parsed INIs.
RmIniSource* RandomMapGenerator::s_iniSource = nullptr;

// ---------------------------------------------------------------------------
// [FA2sp] INI read shims. Same argument order as the Win32 calls they replace,
// so the call sites only had to change name.
// ---------------------------------------------------------------------------
int RmReadInt(const char* section, const char* key, int fallback, const char* path)
{
    if (RandomMapGenerator::s_iniSource != nullptr)
        return RandomMapGenerator::s_iniSource->GetInt(path, section, key, fallback);
    return static_cast<int>(GetPrivateProfileIntA(section, key,
                                                  static_cast<UINT>(fallback), path));
}

unsigned int RmReadString(const char* section, const char* key, const char* fallback,
                          char* out, int outSize, const char* path)
{
    if (out == nullptr || outSize <= 0)
        return 0;
    out[0] = '\0';

    if (RandomMapGenerator::s_iniSource != nullptr)
    {
        // A null key means "list this section's key names", exactly like
        // GetPrivateProfileString.
        if (key == nullptr)
            return static_cast<unsigned int>(
                RandomMapGenerator::s_iniSource->GetKeyList(path, section, out, outSize));

        RandomMapGenerator::s_iniSource->GetString(path, section, key, out, outSize);
        return static_cast<unsigned int>(std::strlen(out));
    }
    return static_cast<unsigned int>(
        GetPrivateProfileStringA(section, key, fallback, out, outSize, path));
}

void RmReadSectionNames(std::vector<std::string>& out, const char* path)
{
    out.clear();

    if (RandomMapGenerator::s_iniSource != nullptr)
    {
        RandomMapGenerator::s_iniSource->GetSectionNames(path, out);
        return;
    }

    std::vector<char> buffer(256 * 1024, '\0');
    const DWORD len = GetPrivateProfileSectionNamesA(
        buffer.data(), static_cast<DWORD>(buffer.size()), path);
    if (len == 0)
        return;

    for (const char* name = buffer.data(); *name; name += std::strlen(name) + 1)
        out.push_back(name);
}

// 界面选定输出文件夹后、开始生成前调用：规整成带结尾反斜杠的绝对路径，
// 目录不存在顺手建出来，并同步给静态 DiagLog。
void RandomMapGenerator::SetOutputDir(const std::wstring& dir)
{
    outputDir_ = dir;
    if (!outputDir_.empty())
    {
        const wchar_t last = outputDir_[outputDir_.size() - 1];
        if (last != L'\\' && last != L'/')
            outputDir_.push_back(L'\\');
        CreateDirectoryW(outputDir_.c_str(), nullptr);   // 已存在会失败，忽略
    }
    s_diagDir_ = outputDir_;
}

RandomMapGenerator::~RandomMapGenerator()
{
    delete[] cellPool_;
    delete[] cellSlots_;
    delete[] workCells_;
}

// ---------------------------------------------------------------------------
// CalcMapSize (replicates 0x599665 - 0x599748)
//
//   i = clamp(playerCount - 2, 0, 6)
//   wFrac = hFrac = sizeSlider / 3.0
//   if LandType != Inland and != Mountainous: clamp both to 1.2
//   W = F2I64(Wmin[i]*(1-wFrac) + Wmax[i]*wFrac)   (visible size)
//   H = F2I64(Hmin[i]*(1-hFrac) + Hmax[i]*hFrac)
//   W' = W + 4, H' = H + 12  (MapRect, sub_42AC00)
//   workSide = W' + H' + 1  (dword_89C2DC)
// ---------------------------------------------------------------------------
MapSizeResult RandomMapGenerator::CalcMapSize(const MapGenConfig& cfg) const
{
    MapSizeResult result = {};

    int playerIdx = cfg.playerCount - 2;
    if (playerIdx < 0) playerIdx = 0;
    if (playerIdx > 6) playerIdx = 6;

    float wFrac = (float)cfg.sizeSlider * kOneThird;
    float hFrac = (float)cfg.sizeSlider * kOneThird;

    RmLandType lt = cfg.landType;
    if (lt != RmLandType::Inland && lt != RmLandType::Mountainous)
    {
        if (wFrac > kSizeClamp) wFrac = kSizeClamp;
        if (hFrac > kSizeClamp) hFrac = kSizeClamp;
    }

    float w = kWmin[playerIdx] * (1.0f - wFrac) + kWmax[playerIdx] * wFrac;
    float h = kHmin[playerIdx] * (1.0f - hFrac) + kHmax[playerIdx] * hFrac;

    result.width     = F2I64(w);
    result.height    = F2I64(h);
    result.mapWidth  = result.width + 4;
    result.mapHeight = result.height + 12;
    result.workSide  = result.mapWidth + result.mapHeight + 1;

    return result;
}

// ---------------------------------------------------------------------------
// InitCells (MapClass::CreateEmptyMap + cell reset 0x599e86 - 0x599eaa)
//
// Diamond existence: W' < x+y <= W'+2H' and |x-y| < W' (CellExists).
// Slot array: 512-stride rows, nullptr outside the diamond - this is the
// sentinel the CellIterator probes to terminate enumeration.
// Cell reset: SlopeIndex=0, Level=4, IsoTileTypeIndex=0, Height=0,
//             OverlayTypeIndex=-1, OverlayData=0.
// ---------------------------------------------------------------------------
void RandomMapGenerator::InitCells()
{
    delete[] cellPool_;
    delete[] cellSlots_;
    cellPool_ = nullptr;
    cellSlots_ = nullptr;
    slotRows_ = 0;
    cellCount_ = 0;

    // Rows cover every diagonal the iterator can touch (incl. the
    // out-of-diamond probe cell that terminates iteration)
    slotRows_ = size_.mapWidth + size_.mapHeight + 1;

    // Row pitch. The retail engine uses 512 unconditionally; sizing it from the
    // map lets w+h exceed 512 (the editor supports up to 2048).
    slotStride_ = slotRows_;

    cellSlots_ = new MapCell*[static_cast<size_t>(slotRows_) * slotStride_];
    std::memset(cellSlots_, 0, sizeof(MapCell*) * static_cast<size_t>(slotRows_) * slotStride_);

    // x spans [1, W'+H'-1] inside the diamond (x = 0 has no solution)
    int maxX = size_.mapWidth + size_.mapHeight - 1;

    for (int y = 0; y < slotRows_; ++y)
        for (int x = 0; x <= maxX; ++x)
            if (CellExists(x, y))
                ++cellCount_;

    cellPool_ = new MapCell[cellCount_];

    int idx = 0;
    for (int y = 0; y < slotRows_; ++y)
    {
        for (int x = 0; x <= maxX; ++x)
        {
            if (!CellExists(x, y))
                continue;
            MapCell& cell = cellPool_[idx];
            cell.MapCoords = x | (y << 16);
            cell.Level = baseLevel_;
            cell.IsoTileTypeIndex = 0;
            cell.SlopeIndex = 0;
            cell.Height = 0;
            cell.OverlayTypeIndex = -1;
            cell.OverlayData = 0;
            cell.AltFlags = 0;
            cellSlots_[static_cast<size_t>(slotStride_) * y + x] = &cellPool_[idx];
            ++idx;
        }
    }
}

// ---------------------------------------------------------------------------
// InitWorkArray (0x59a30a - 0x59a37f)
//   80 bytes per cell, workSide * workSide cells.
//   WorkCell ctor: bytes 64..67 = -1 (data[16]), byte 74 = 1.
// ---------------------------------------------------------------------------
void RandomMapGenerator::InitWorkArray()
{
    delete[] workCells_;
    const int workCount = size_.workSide * size_.workSide;
    workCells_ = new WorkCell[workCount];

    // The two parallel cell-attribute arrays MouseClass::Instance carries
    // (indexed through sub_56D3F0 = our PassabilityIndex) share the work
    // array's shape. Vanilla allocates them together with the map; the
    // Recalculating-cell-attributes stage is the first to write them.
    //
    // The passability byte starts at OutsideMap (7), NOT 0. Every slot outside
    // the diamond keeps that value - RecalculateCellAttributes only walks the
    // diamond - and the zone rebuild of sub_56C510 relies on it: its walk starts
    // at the array's first slot and treats "byte 0 == 7" as the only stop
    // condition, so an outside slot reading 0 lets FillZoneFromRow start on the
    // array's first row and walk its north pass off the front of the buffer.
    levelAndPassability_.assign(static_cast<size_t>(workCount),
                                CellLevelPassability{
                                    static_cast<uint8_t>(PassabilityType_OutsideMap),
                                    0, 0 });
    lastRecordedZone_ = 0;
    for (int i = 0; i < 256; ++i)
        zoneConnections_[i].clear();
    passabilityCopy_.assign(static_cast<size_t>(workCount), 0);
    levelAndPassability2_.assign(static_cast<size_t>(workCount), 0);

    // The starting-point ledger. The vanilla's ScenarioClass::Waypoints is a
    // 702-entry array that the scenario load zeroes; nothing writes it before
    // the starting-point stage, so an all-zero array is the same starting state.
    waypoints_.assign(702, CellStruct{ 0, 0 });
    startingPoints_.clear();
    startingPointCount_ = 0;
}

// ---------------------------------------------------------------------------
// FillWorkCoords (0x59a385 - 0x59a3dc)
//   Enumerate cells in iterator (diagonal) order, write MapCoords into
//   workArray[x + workSide * y].
// ---------------------------------------------------------------------------
void RandomMapGenerator::FillWorkCoords()
{
    if (!workCells_ || !cellSlots_)
        return;

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())
    {
        int x = cell->MapCoords & 0xFFFF;
        int y = (uint32_t)cell->MapCoords >> 16;
        workCells_[x + size_.workSide * y].MapCoords() = cell->MapCoords;
    }
}

// ---------------------------------------------------------------------------
// GenerateMapBody - RNG-relevant core of sub_599650 / sub_598960
//
//   1. rng_.Seed(seed)         (qmemcpy(&dst_, sub_65C6D0(v39, this[29])))
//                              + GaussState reset at 0xABDFB8 (in Seed)
//   2. ReInitMapData()         (sub_5981F0, no RNG consumption)
//   3. CalcMapSize()           (0x599665 - 0x599748)
//   4. InitCells()             (CreateEmptyMap semantics)
//   5. InitWorkArray()         (0x59a30a - 0x59a37f)
//   6. FillWorkCoords()        (0x59a385 - 0x59a3dc)
//   7. random25Flag_           (0x59a4cc, exactly ONE Random() call)
//
// Verified via dst_ xrefs: no other RNG consumer runs between the seeding
// and sub_59A6C0 terrain generation.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::GenerateMapBody(const MapGenConfig& cfg)
{
    DiagLog("==== MAP BODY START ====");
    config_ = cfg;

    // Water amount (this[19]): rolled by the caller, same as vanilla rolls
    // it in the dialog layer (sub_597260) before generation starts.
    waterAmount_ = cfg.waterAmount;

    // The map's own Randomizer is seeded with the CONSTANT 0 in the vanilla
    // (0x58b770: `push 0; mov ecx, offset dword_ABE890; call sub_65C6D0`).
    // The UI seed box keeps that default (mapRngSeed == 0); a non-zero value is
    // a debug override that re-sequences the whole terrain body.
    (void)cfg.randomSeed;
    rng_.Seed(cfg.mapRngSeed);

    ReInitMapData();

    if (cfg.overrideMapWidth > 0 && cfg.overrideMapHeight > 0)
    {
        // [FA2sp] Explicit extent (sub-region painting). Keep the same
        // W' = W + 4 / H' = H + 12 relationship CalcMapSize produces, so the
        // rest of the pipeline sees a perfectly ordinary map.
        size_.mapWidth = cfg.overrideMapWidth;
        size_.mapHeight = cfg.overrideMapHeight;
        size_.width = size_.mapWidth - 4;
        size_.height = size_.mapHeight - 12;
        size_.workSide = size_.mapWidth + size_.mapHeight + 1;
    }
    else
    {
        size_ = CalcMapSize(cfg);
    }
    if (size_.width <= 0 || size_.height <= 0)
        return false;

    baseLevel_ = 4;
    usedWaterCells_ = 0;    // RMG this[193]: per-map river/lake budget counter

    // Theater tile families always come from TEMPERATMD.INI / SNOWMD.INI - the
    // values must match what FA2 loaded, and the file ships loose in the game
    // folder (s_gameIniDir points there under the editor).
    LoadTheaterTiles(cfg.theater);   // IsometricTileTypeClass::ReadINI [General]

    // [FA2sp] The TMP cell attributes are the only table that cannot come from
    // an INI: the host extracts each TMP out of the game MIX and pushes the
    // parsed vectors through SetTileCellAttrs(). Everything else below still
    // reads the game folder through ResolveGameIni().
    if (!s_bUseHostData)
    {
        // The data the "Recalculating cell attributes" stage reads.
        // LoadTileCellAttrs needs the tile ranges LoadTheaterTiles just
        // produced; the other four come from the rules INI.
        LoadTileCellAttrs(cfg.theater);
    }

    LoadOverlayTypes();              // OverlayTypeClass::LoadFromINI
    LoadGroundTypes();               // GroundType::Array
    LoadTiberiums();                 // TiberiumClass::LoadFromINI
    LoadNeutralTechBuildings();      // RulesClass::NeutralTechBuildings + art
    LoadMultiplayerHouses();         // rules [Countries] for the .yrm [Houses]

    InitCells();
    InitWorkArray();
    FillWorkCoords();

    random25Flag_ = rng_.Chance25();

    return true;
}

// ---------------------------------------------------------------------------
// GenerateTerrain - sub_59A6C0 segments 1 + 2
//
// Segment 1 (0x59a724 - 0x59a738): flood every diamond cell with water.
//   sub_578350(&MouseClass::Instance);            // reset iterator
//   for ( i = CellIteratorNext(); i; i = CellIteratorNext() )
//     i->IsoTileTypeIndex = nIdx;                 // WaterSet base tile
//
// Segment 2 (0x59a6f6 - 0x59a718): dispatch by this[15] (LandType):
//   0 -> sub_59AD10 (Archipelago)
//   1 -> sub_59AFA0 (Continent)
//   2 -> sub_59B200 (TeamContinent)
// LandType 3/4 (Inland/Mountainous) never reach sub_59A6C0 in vanilla
// (sub_598960 routes them to sub_59C580) and fall through, mirrored here.
//
// Segments 3-6 close the terrain stage:
//   3 (0x59a722) fill single-cell water holes;
//   4 (0x59a788) whole-map shore pass sub_57A0C0(0, 0);
//   5 (0x59a794) reset both work marks to -1, drop the island list;
//   6 (0x59a884) green the bare land that touches a shore tile.
// ---------------------------------------------------------------------------
void RandomMapGenerator::GenerateTerrain()
{
    // Segment 1: flood with water
    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())
        cell->IsoTileTypeIndex = waterTileIndex_;

    // Segment 2: land type dispatch (generator bodies pending)
    switch (config_.landType)
    {
    case RmLandType::Archipelago:
        GenerateArchipelago();
        break;
    case RmLandType::Continent:
        GenerateContinent();
        break;
    case RmLandType::TeamContinent:
        GenerateTeamContinent();
        break;
    default:
        break;
    }

    // Segment 3 (0x59a722 - 0x59a770): a water cell whose four orthogonal
    // neighbours all carry no real tile is a one-cell hole - make it land.
    CellIterator holeIt;
    holeIt.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = holeIt.Next())
    {
        if (cell->IsoTileTypeIndex != waterTileIndex_)            // 0x59a741
            continue;
        bool allPlaceholder = true;                              // 0x59a743
        for (int facing = 0; facing < 8; facing += 2)            // 0x59a745
        {
            if (!IsPlaceholderTile(GetNeighbourCell(cell, facing)))    // 0x59a751
            {
                allPlaceholder = false;
                break;
            }
        }
        if (allPlaceholder)
            cell->IsoTileTypeIndex = 0;                          // 0x59a768
    }

    // Segment 4 (0x59a788): the whole-map shore pass. genCode 0 matches every
    // cell, so this is what actually paints the coastline of the land grown
    // above. GrowPatch's own sub_57A0C0(genCode, 1) call is followed by a
    // reclaim sweep (0x59c449) that turns those temporary shore tiles back
    // into bare land on purpose, so the shores have to be repainted here.
    SmoothWaterBody(0, 0);                                       // sub_57A0C0(0, 0)

    // Segment 5 (0x59a794 - 0x59a7c0): reset both work marks over the whole
    // work grid. 0x59a7cc - 0x59a86a then frees the island list hanging off
    // dword_ABDF94 and clears dword_ABED14; that list is only produced by the
    // archipelago generator (sub_59AD10), which is not ported, so there is
    // nothing to free here.
    if (workCells_ != nullptr)                                   // 0x59a794
    {
        const int workCellCount = size_.workSide * size_.workSide;    // 0x59a79c
        for (int idx = 0; idx < workCellCount; ++idx)            // 0x59a7a3
        {
            workCells_[idx].data[14] = -1;                       // +56, 0x59a7ae
            workCells_[idx].data[15] = -1;                       // +60, 0x59a7b8
        }
    }

    // Segment 6 (0x59a884 - 0x59a8db): every cell carrying a shore tile that
    // still has bare neighbours stamps those neighbours with the green ground
    // tile, so the land behind the shore is not left as a placeholder.
    CellIterator greenIt;
    greenIt.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = greenIt.Next())
    {
        const int tile = cell->IsoTileTypeIndex;
        if (tile < shorePieces_ || tile >= shorePieces_ + 42)    // 0x59a89b
            continue;
        for (int facing = 0; facing < 8; facing += 2)            // 0x59a8a4
        {
            MapCell* neighbour = GetNeighbourCell(cell, facing);     // 0x59a8ae
            if (IsPlaceholderTile(neighbour))                    // 0x59a8b2
                neighbour->IsoTileTypeIndex = greenTileIndex_;   // 0x59a8c0
        }
    }
}

// ---------------------------------------------------------------------------
// ClearWorkOccupancy - common generator prologue
//
// Vanilla (e.g. sub_59AD10 @ 0x59ad5e, sub_59AFA0 @ 0x59b043,
// sub_59B200 @ 0x59b063):
//   sub_578350(&MouseClass::Instance);
//   for ( i = CellIteratorNext(); i; i = CellIteratorNext() )
//     *(dword_ABED10 + 20 * i->MapCoords
//       + 20 * dword_89C2DC * HIWORD(i->MapCoords) + 15) = 0;
// (dword +15 = byte 60 = work cell data[15], the occupancy mark)
// ---------------------------------------------------------------------------
void RandomMapGenerator::ClearWorkOccupancy()
{
    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())
    {
        int x = cell->MapCoords & 0xFFFF;
        int y = (uint32_t)cell->MapCoords >> 16;
        workCells_[x + size_.workSide * y].data[15] = 0;
    }
}

// The [Map] LocalSize offsets - VisibleRect.X / VisibleRect.Y - that the
// VisibleRect-derived bounds use (same pair MapGenStartpoint.cpp and
// MapGenMakingSub.cpp carry; W' / H' are size_.width / size_.height).
static const int kVisibleOffsetX = 2;
static const int kVisibleOffsetY = 5;

// ---------------------------------------------------------------------------
// Land type generators - all three implemented.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// GenerateArchipelago (sub_59AD10) - LandType 0 "archipelago".
//
// The visible rectangle is carved into a ragged grid of rectangles
// (sub_59A8F0), then every rectangle grows one organic island from its own
// diamond centre, so the map ends up as a scatter of islands instead of one
// mass. The block count is playerCount + rand(1 .. max(playerCount/2, 2)), so
// bigger matches get more islands.
//
//   0x59ad37  this[194] = 1                        ; genCode_ = 1
//   0x59ad3d  span = max(this[20] / 2, 2)          ; this[20] = player count
//   0x59ad5b  extra = F2I64(Random()*span + 1)     ; 1 .. span
//   0x59ad6f  sub_59A8F0(blocks, extra + playerCount, VisibleRect)
//   0x59ad74  ClearWorkOccupancy()                 ; work data[15] = 0
//   0x59ad9c  for each block rect:
//               area   = F2I64((Random()*0.05 + 0.45) * 2*W*H)   ; ~90-100% of W*H
//               centre = block centre in diamond coords (seed == centre)
//               do {                                             ; <= 10 retries
//                   grow = GrowPatch(area, &rect, centre, 1, centre, 0.25, 0)
//                   if (!grow) ++genCode_
//               } while (!grow)
//
// Unlike the continent types, GrowPatch gets step 0.25 / priority mode 0, and
// the bounds are the block's own visible-rect rect in ellipse mode (1).
// ---------------------------------------------------------------------------
void RandomMapGenerator::GenerateArchipelago()
{
    genCode_ = 1;                                       // 0x59ad37

    // Block count: playerCount + rand(1 .. max(playerCount/2, 2)).
    int span = config_.playerCount / 2;                 // 0x59ad3d
    if (span < 2)
        span = 2;                                       // 0x59ad4a
    const int extra = rng_.RandomFloatRange(1, span);   // 0x59ad5b

    // The visible rect: same origin/size the other land types use.
    const MapRectTag visible =                          // v26
    {
        kVisibleOffsetX,
        kVisibleOffsetY,
        size_.width,                                    // VisibleRect.Width
        size_.height                                    // VisibleRect.Height
    };

    std::vector<MapRectTag> blocks;
    SplitVisibleRectIntoBlocks(blocks, extra + config_.playerCount,  // 0x59ad6f
                               visible);

    ClearWorkOccupancy();                               // 0x59ad74

    // Grow one organic island per block, in the order sub_59A8F0 emitted them.
    for (size_t b = 0; b < blocks.size(); ++b)          // 0x59ad9c
    {
        const MapRectTag rect = blocks[b];              // v22..v25

        // Island budget: 45%-50% of twice the rect area, i.e. ~90%-100% of W*H.
        const int area = F2I64(                         // 0x59adcb
            (rng_.Unit() * 0.05 + 0.45)
            * (2.0 * rect.Width * rect.Height));        // v19

        // Block centre in diamond coords; both the seed and GrowPatch's
        // directional-priority origin (arg 5) sit on it.
        const int centreX =                             // 0x59add6
            rect.X + rect.Width / 2 + rect.Height / 2 + rect.Y + 1;
        const int centreY =                             // 0x59adcf
            rect.Y + size_.mapWidth + rect.Height / 2 - rect.Width / 2 - rect.X;
        const int centrePacked = centreX | (centreY << 16);

        // Up to 10 tries per block; every failed grow burns a generation code.
        int grown = 0;                                  // v13
        for (int attempt = 0; attempt < 10; ++attempt)  // n10_1
        {
            grown = GrowPatch(area, &rect, centrePacked, // 0x59ade4
                              1, centrePacked, 0.25, 0);
            if (grown)
                break;
            ++genCode_;                                 // 0x59adeb
        }
    }
}

// ---------------------------------------------------------------------------
// Block-grid split for GenerateArchipelago (sub_59A8F0 @ 0x59A8F0).
//
// Cuts `rect` (the visible rectangle) into exactly `count` rectangles packed
// in a ragged grid, appended to `blocks` in group order.
//
//   root = floor(sqrt(count))                                    ; v4 / v31
//   a    = root, or root+1 when count is not a perfect square     ; v5 / v26
//   b    = a when root*a < count, else root                       ; v6 / v23
//
// The grid is b groups of a cells, one output rect per cell. To hit `count`
// exactly, (a*b - count) randomly chosen groups are shortened to root (= a-1)
// cells (v8); a shortened group is nudged half a cell so its gap is centred.
//
// A coin flip picks which axis is the fine one:
//   * rand >= 0.5 : b rows, each up to a cells wide; rect = W/a x H/b,
//                   cells step in X, rows step in Y, X resets per row.
//   * rand <  0.5 : b columns, each up to a cells tall; rect = W/b x H/a,
//                   cells step in Y, columns step in X, Y resets per column.
//
// Every emitted rect is inset by 2 cells each side: {X+2, Y+2, w-4, h-4}.
// ---------------------------------------------------------------------------
void RandomMapGenerator::SplitVisibleRectIntoBlocks(std::vector<MapRectTag>& blocks,
                                                    int count,
                                                    const MapRectTag& rect)
{
    if (count <= 0)
        return;

    const int root = static_cast<int>(std::sqrt(static_cast<double>(count)));
    const int a = (root * root == count) ? root : root + 1;
    const int b = (root * a < count) ? a : root;

    // Coin flip: fine split on X (rows) or on Y (columns).
    const bool wide = (rng_.Unit() >= 0.5);

    // One output rect's size and the two step sizes used while walking the
    // grid: inside a group, and from one group to the next.
    int blockW;         // v24
    int blockH;         // low dword of v25
    int innerStepX;     // HIDWORD(v25)
    int innerStepY;     // v27
    int groupStepX;     // v29
    int groupStepY;     // v30
    if (wide)
    {
        blockW = rect.Width / a;
        blockH = rect.Height / b;
        innerStepX = blockW;
        innerStepY = 0;
        groupStepX = 0;
        groupStepY = blockH;
    }
    else
    {
        blockW = rect.Width / b;
        blockH = rect.Height / a;
        innerStepX = 0;
        innerStepY = blockH;
        groupStepX = blockW;
        groupStepY = 0;
    }

    // b groups of a cells; shorten (a*b - count) random groups to root cells.
    std::vector<int> perGroup(static_cast<size_t>(b), a);       // v44
    std::vector<int> pool(static_cast<size_t>(b));              // Block
    for (int i = 0; i < b; ++i)
        pool[static_cast<size_t>(i)] = i;

    const int shortGroups = a * b - count;                      // v8
    for (int s = 0; s < shortGroups; ++s)
    {
        const int pick = rng_.RandomFloatRange(
            0, static_cast<int>(pool.size()) - 1);
        perGroup[static_cast<size_t>(pool[static_cast<size_t>(pick)])] = root;
        pool.erase(pool.begin() + pick);
    }

    // Walk the grid in group order and emit one rect per cell.
    int x = rect.X;                                             // v33
    int y = rect.Y;                                             // v34
    for (int g = 0; g < b; ++g)
    {
        const int inGroup = perGroup[static_cast<size_t>(g)];   // v17
        if (inGroup < a)                                        // short group
        {
            if (wide)
                x += blockW / 2;                                // v24 / 2
            else
                y += blockH / 2;                                // v25 / 2
        }
        for (int k = 0; k < inGroup; ++k)
        {
            MapRectTag out;
            out.X = x + 2;
            out.Y = y + 2;
            out.Width = blockW - 4;
            out.Height = blockH - 4;
            blocks.push_back(out);
            x += innerStepX;
            y += innerStepY;
        }
        x += groupStepX;
        y += groupStepY;
        if (wide)
            x = rect.X;
        else
            y = rect.Y;
    }
}

// ---------------------------------------------------------------------------
// GenerateContinent (sub_59AFA0) - LandType 1 "continent" (single land mass).
//
// One organic land mass is grown from an ellipse until the land covers `target`
// of the map, up to 100 patches. Every patch is seeded at the still-water cell
// nearest (Manhattan) to the map centre, so the mass keeps closing around a
// single core instead of scattering islands.
//
//   0x59afb3  this[194] = 1                       ; genCode_ = 1
//   0x59afbd  total = 2 * MapRect.Width * (MapRect.Height + 4)   (sub_42B1F0)
//   0x59afc2  water  = this[19]                   ; water amount, percent
//   0x59affb  target = (1 - water*0.01)*(0.5-0.45) + 0.45        ; 0.45 .. 0.50
//   0x59b016  maxPatch = F2I64(total * 0.03 * target)            ; 3% of total
//   0x59b030  centre = MapRect.Height/2 + MapRect.Width/2
//   0x59b038  seed  = (centre+1, centre)          ; first patch = map centre
//   0x59b043  ClearWorkOccupancy()                ; work data[15] = 0, whole map
//   0x59b0af  bounds = VisibleRect inset by 1     -> {3, 6, W-2, H-2}
//   0x59b0ca  if (target > 0) do {
//   0x59b0db      if (patches >= 100) break;
//   0x59b0ed      area = min(F2I64((target - covered) * total), maxPatch)
//   0x59b133      grown += GrowPatch(area, &bounds, seed, 1, centre, 0.75, 1)
//   0x59b15b      covered = grown / total
//   0x59b172      seed = nearest unoccupied cell to centre (Manhattan abs)
//   0x59b1ef  } while (covered < target);
//
// GrowPatch's directional-priority origin (arg 5) is the packed cell
// (centre+1, centre) - the same point the first patch is seeded from.
// ---------------------------------------------------------------------------
void RandomMapGenerator::GenerateContinent()
{
    genCode_ = 1;                                       // 0x59afb3

    // Total cell budget of the diamond (sub_42B1F0 @ 0x42b1f0).
    const double total = static_cast<double>(2 * size_.mapWidth *
                                             (size_.mapHeight + 4));  // 0x59afbd

    // Target land fraction: more water -> slightly less land, 0.50 down to 0.45.
    const double water = static_cast<double>(waterAmount_);           // 0x59afc2
    const double target = (1.0 - water * 0.01) * (0.5 - 0.45) + 0.45; // 0x59affb
    const int maxPatch = F2I64(total * 0.03 * target);                // 0x59b016

    // The map is a rotated square: its middle cell sits at (Height/2 + Width/2)
    // on both axes (0x59b030).
    const int centre = size_.mapHeight / 2 + size_.mapWidth / 2;

    int seedX = centre + 1;                             // 0x59b03f
    int seedY = centre;                                 // 0x59b038
    const int centrePacked =                             // 0x59b119 / 0x59b11e
        (centre + 1) | (centre << 16);

    ClearWorkOccupancy();                               // 0x59b043

    // Bounds handed to GrowPatch: the visible rect inset by one cell each side.
    MapRectTag bounds;
    bounds.X      = kVisibleOffsetX + 1;                // 0x59b0af
    bounds.Y      = kVisibleOffsetY + 1;                // 0x59b0bb
    bounds.Width  = size_.width - 2;                    // 0x59b0c2
    bounds.Height = size_.height - 2;                   // 0x59b0c6

    int grown = 0;                                      // v12
    int patches = 0;                                    // n100
    double covered = 0.0;                               // v18

    if (target > 0.0)                                   // 0x59b0ca
    {
        do
        {
            if (patches >= 100)                         // 0x59b0db
                break;

            // Remaining shortfall, capped at 3% of the map per patch.
            int area = F2I64((target - covered) * total);        // 0x59b0ed
            if (area >= maxPatch)                       // 0x59b0f8
                area = maxPatch;                        // 0x59b0fa

            grown += GrowPatch(area, &bounds,            // 0x59b133
                               seedX | (seedY << 16),
                               1, centrePacked, 0.75, 1);
            ++patches;                                  // 0x59b137
            covered = grown / total;                    // 0x59b15b

            // Next seed: the still-unoccupied cell closest (Manhattan) to the
            // centre, so each new patch grows against the existing mass.
            int bestDistance = 50000;                   // 0x59b144
            int bestX = 0;                              // 0x59b149 (MapCoords = 0)
            int bestY = 0;
            CellIterator it;                             // 0x59b15f
            it.Reset(cellSlots_, size_.mapWidth, slotStride_);
            while (MapCell* cell = it.Next())
            {
                const int x = cell->MapCoords & 0xFFFF;
                const int y = static_cast<uint32_t>(cell->MapCoords) >> 16;
                if (workCells_[x + size_.workSide * y].data[15] != 0)
                    continue;                           // already land, skip

                const int distance = std::abs(x - (centre + 1)) +
                                     std::abs(y - centre);        // 0x59b1b1
                if (distance < bestDistance)            // 0x59b1b5
                {
                    bestDistance = distance;            // 0x59b1ba
                    bestX = x;                          // 0x59b1bc
                    bestY = y;
                }
            }
            seedX = bestX;                              // 0x59b1e2
            seedY = bestY;                              // 0x59b1e6
        }
        while (covered < target);                       // 0x59b1ef
    }
}

// ---------------------------------------------------------------------------
// GenerateTeamContinent (sub_59B200) - LandType 2 "team continent".
//
// The map is cut into two halves and every half grows its own land mass, so the
// teams start on separate continents. A single coin flip picks the cut: two
// horizontal bands (top / bottom) or two vertical bands (left / right). Inside a
// half every patch is seeded at the still-water cell that passes the half's
// ellipse test and is nearest (weighted Manhattan) to the half's centre, so each
// mass keeps closing around its own core.
//
//   0x59b200  this[194] = 1                       ; genCode_ = 1
//   0x59afbd  total = 2 * MapRect.Width * (MapRect.Height + 4)   (sub_42B1F0)
//   0x59b03c  target = (1 - water*0.01)*(0.2-0.15) + 0.15        ; 0.15 .. 0.20
//   0x59b03e  maxPatch = F2I64(total * 0.06 * target)            ; 6% of total
//   0x59b043  ClearWorkOccupancy()                ; work data[15] = 0, whole map
//   0x59b046  roll = Random(); roll >= 0.5 -> top/bottom split, else left/right
//   0x59b0??  for each of the two half rects:                    ; exactly 2
//               centre = half centre in diamond coords; first seed == centre
//               do {
//                   if (patches >= 100) break;                   ; shared cap
//                   area = min(F2I64((target - covered) * total), maxPatch)
//                   grown += GrowPatch(area, &half, seed, 1, centre, 0.75, 1)
//                   covered = grown / total
//                   seed = nearest unoccupied cell of the half that passes the
//                          half ellipse (weighted Manhattan; 0 -> GrowPatch
//                          falls back to a random water cell)
//               } while (covered < target)
//
// Both halves share `patches`, so the two masses together stay under 100
// patches. GrowPatch's directional-priority origin (arg 5) is the half centre.
// ---------------------------------------------------------------------------
void RandomMapGenerator::GenerateTeamContinent()
{
    genCode_ = 1;                                       // 0x59b200

    // Total cell budget of the diamond (sub_42B1F0 @ 0x42b1f0).
    const double total = static_cast<double>(2 * size_.mapWidth *
                                             (size_.mapHeight + 4));
    const double water = static_cast<double>(waterAmount_);           // 0x59b03c
    // Team continents keep less land than a single continent: 0.20 down to 0.15.
    const double target = (1.0 - water * 0.01) * (0.2 - 0.15) + 0.15;
    const int maxPatch = F2I64(total * 0.06 * target);                // 0x59b03e

    ClearWorkOccupancy();                               // 0x59b043

    // Coin flip: two horizontal bands (top / bottom) or two vertical bands.
    MapRectTag half[2];                                 // v40, then X/Y/W_1/H_1
    const double roll =
        static_cast<double>(static_cast<uint32_t>(rng_.Next())) * kUnitScale;
    if (roll >= 0.5)                                    // 0x59b069
    {
        half[0].X      = kVisibleOffsetX;                // v40[0]
        half[0].Y      = kVisibleOffsetY;                // v40[1]
        half[0].Width  = size_.width;                    // 0x59b06d
        half[0].Height = size_.height / 2 - 1;           // 0x59b071
        half[1].X      = half[0].X;
        half[1].Y      = size_.height / 2 + kVisibleOffsetY + 1;   // 0x59b079
        half[1].Width  = half[0].Width;
        half[1].Height = half[0].Height;
    }
    else
    {
        half[0].X      = kVisibleOffsetX;                // v40[0]
        half[0].Y      = kVisibleOffsetY;                // v40[1]
        half[0].Width  = size_.width / 2 - 1;            // 0x59b08c
        half[0].Height = size_.height;                   // 0x59b088
        half[1].X      = size_.width / 2 + kVisibleOffsetX + 1;    // 0x59b095
        half[1].Y      = half[0].Y;
        half[1].Width  = half[0].Width;
        half[1].Height = half[0].Height;
    }

    int patches = 0;                                    // n100, shared by both halves

    for (int h = 0; h < 2; ++h)
    {
        const MapRectTag& r = half[h];
        const int rectX = r.X;                          // v26
        const int rectY = r.Y;                          // v27
        const int rectW = r.Width;                      // v28
        const int rectH = r.Height;                     // v29

        // Half centre in diamond coords. The first seed of the half is the centre
        // itself and the directional-priority origin stays there all along.
        const int centreX = rectX + rectH / 2 + rectW / 2 + rectY + 1;  // p_n2_1
        const int centreY = rectY + size_.mapWidth + rectH / 2 - rectW / 2 - rectX; // v33
        const int centrePacked = centreX | (centreY << 16);

        // Weighted-Manhattan axis weights for the seed pick (0x59b0d6-0x59b108).
        double weightX;                                 // v30
        double weightY;                                 // v49
        if (rectH <= rectW)                             // 0x59b0d6
        {
            weightX = 1.0;
            weightY = static_cast<double>(rectW) / rectH * 1.2;
        }
        else
        {
            weightY = 1.0;
            weightX = static_cast<double>(rectH) / rectW * 1.2;
        }
        const double invW2 = 1.0 / ((rectW * 0.5) * (rectW * 0.5));   // v53
        const double invH2 = 1.0 / ((rectH * 0.5) * (rectH * 0.5));   // v52

        int grown = 0;                                  // v22
        double covered = 0.0;                           // v39
        int seedX = centreX;                            // p_n2
        int seedY = centreY;                            // v37

        if (target > 0.0)                               // 0x59b10c
        {
            do
            {
                if (patches >= 100)                     // 0x59b10e
                    break;

                // Remaining shortfall, capped at 6% of the map per patch.
                int area = F2I64((target - covered) * total);        // 0x59b11b
                if (area >= maxPatch)                   // 0x59b122
                    area = maxPatch;

                grown += GrowPatch(area, &r, seedX | (seedY << 16),  // 0x59b129
                                   1, centrePacked, 0.75, 1);
                int bestSeed = 0;                       // p_n2_2 (MapCoords = 0)
                ++patches;                              // 0x59b131
                covered = grown / total;                // 0x59b137

                // The half's diamond bounding box, in (x+y) / (x-y) axes.
                const int boxMinDiff = 2 * rectX - size_.mapWidth + 1;   // v6
                const int boxMaxDiff = boxMinDiff + 2 * rectW + 2;       // v47
                const int boxMinSum  = size_.mapWidth + 2 * rectY + 1;   // v8
                const int boxMaxSum  = boxMinSum + 2 * rectH + 2;        // v51
                int bestDistance = 50000;               // n50000

                // Scan the whole work grid (vanilla walks the 80-byte work array).
                const int cellCount = size_.workSide * size_.workSide;   // 0x59b12b
                for (int idx = 0; idx < cellCount; ++idx)                // 0x59b178
                {
                    const int x = idx % size_.workSide;
                    const int y = idx / size_.workSide;
                    const int sum = x + y;              // v12
                    const int diff = x - y;             // v13
                    if (sum < boxMinSum || sum > boxMaxSum)              // 0x59b18d
                        continue;
                    if (diff < boxMinDiff || diff > boxMaxDiff)          // 0x59b1a1
                        continue;
                    if (workCells_[idx].data[15] != 0)  // 0x59b1ab already land
                        continue;

                    const int packed = x | (y << 16);
                    if (!PatchInBounds(packed, &r, 1, invW2, invH2))     // 0x59b1b9
                        continue;

                    const int distance =                     // 0x59b1c3
                        F2I64(std::abs(y - centreY) * weightY
                              + std::abs(x - centreX) * weightX);
                    if (distance < bestDistance)        // 0x59b1e2
                    {
                        bestDistance = distance;
                        bestSeed = packed;
                    }
                }

                // 0 -> GrowPatch rejected-samples a random water cell instead.
                seedX = static_cast<int16_t>(bestSeed & 0xFFFF);          // 0x59b1eb
                seedY = static_cast<int16_t>(
                    static_cast<uint32_t>(bestSeed) >> 16);               // 0x59b1ef
            }
            while (covered < target);                   // 0x59b1f8
        }
    }
}

// ---------------------------------------------------------------------------
// GrowPatch support - the four orthogonal steps (Neighbours table order, the
// same order GetNeighbourCell uses; the walk advances facing by 2, 0x59c04b).
// ---------------------------------------------------------------------------
namespace
{
static const int16_t kPatchDirX[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
static const int16_t kPatchDirY[8] = {-1, -1,  0,  1,  1,  1,  0, -1 };

// Priority-queue node (vanilla Block, allocated at 0x59bbf0): 8 bytes - the
// packed cell coordinates plus the float priority at +4 (0x59c233 / 0x59c247).
// Nodes are handed out in order and never recycled; the heap stores pointers
// into the node pool, which is sized once up front and never resized.
struct PatchNode
{
    CellStruct coords;     // +0  (low16 X, high16 Y)
    float      priority;   // +4
};

// 1-based min-heap over PatchNode* (vanilla heap control block, 0x14 bytes:
// { count, capacity, slots, maxSeen, minSeen }; the two "seen" pointers are dead
// stores in the vanilla code and are dropped here). Insertion is refused once
// count + 1 reaches the capacity (strictly less than, 0x59bf24 / 0x59c25a).
struct PatchHeap
{
    std::vector<PatchNode*> slots;    // slots[0] unused; slots[1..count] live
    size_t count = 0;

    // Sift-down (sub_5AD870): restore the heap order at `a2` after the root was
    // overwritten by the last slot. Children of i are 2i / 2i+1; the smaller
    // child bubbles up until both children are larger (or absent).
    void SiftDown(size_t a2)
    {
        size_t v2 = a2;
        size_t v3 = 2 * a2;
        const size_t v4 = 2 * a2 + 1;
        if (!(2 * a2 <= count) ||
            slots[a2]->priority <= slots[2 * a2]->priority)
            v3 = a2;
        if (v4 <= count && slots[v3]->priority > slots[v4]->priority)
            v3 = 2 * a2 + 1;
        if (v3 == a2)
            return;
        do
        {
            const size_t v6 = 2 * v3 + 1;
            PatchNode* v7 = slots[v2];
            slots[v2] = slots[v3];
            v2 = v3;
            slots[v3] = v7;
            if (2 * v3 <= count &&
                slots[v3]->priority > slots[2 * v3]->priority)
                v3 *= 2;
            if (v6 <= count && slots[v3]->priority > slots[v6]->priority)
                v3 = v6;
        }
        while (v3 != v2);
    }

    // Sift-up insert (vanilla inline: seed push 0x59bf24, neighbour push
    // 0x59c25a). Silently dropped once count + 1 would reach the capacity.
    void Push(PatchNode* node, int capacity)
    {
        size_t v = count + 1;
        if (static_cast<int>(v) >= capacity)
            return;
        size_t parent = v >> 1;
        while (v > 1 && slots[parent]->priority > node->priority)
        {
            slots[v] = slots[parent];
            v = parent;
            parent >>= 1;
        }
        slots[v] = node;
        ++count;
    }

    // Pop the minimum-priority node (vanilla inline: swap root with the last
    // slot, shrink, then sub_5AD870).
    PatchNode* Pop()
    {
        if (count == 0)
            return nullptr;
        PatchNode* root = slots[1];
        slots[1]     = slots[count];
        slots[count] = nullptr;
        --count;
        SiftDown(1);
        return root;
    }
};
}  // namespace

// ---------------------------------------------------------------------------
// GrowPatch - sub_59BBC0 organic patch growth engine.
//
// Grows one organic land patch over the still-water cells: a min-heap holds the
// frontier, the top cell becomes land, its four orthogonal water neighbours are
// pushed with a priority, and a gaussian-drifting cursor steers the growth, so
// the patch shape is the wetting footprint of the cursor walk.
//
//   A 0x59bbcc  area = max(area, 400); capacity = max(8*area + 2, 100)
//   B 0x59bc7c  seed = seedXY, or a random water cell (200 tries, then fail)
//   C 0x59be02  aspect weights (v85/v86) + cursor = seed + seed push + pop
//               + ellipse reciprocals (1/(W/2)^2, 1/(H/2)^2)
//   D 0x59bfe4  growth loop: mark land, IsoTileTypeIndex = 0, push water
//               neighbours, advance cursor by one 2-D gaussian step, pop
//   E 0x59c332  drain the leftover queue (frontier fringe becomes land too)
//   F 0x59c405  SmoothWaterBody(genCode, 1), then reclaim the shore family cells
//   G 0x59c486  success: return cells grown, ++genCode_; failure: roll back
//               every cell marked with this generation to plain water
// Full annotated walkthrough: sub_59BBC0_注释.md.
// ---------------------------------------------------------------------------
int RandomMapGenerator::GrowPatch(int area, const MapRectTag* bounds,
                                  int seedXY, int boundsMode,
                                  int centerXY, double step,
                                  int priorityMode)
{
    // A. Capacity (0x59bbcc) - the 400 floor also raises the loop target.
    if (area <= 400)                                        // 0x59bbd2
        area = 400;
    int capacity = 8 * area + 2;                            // 0x59bbdc
    if (capacity <= 100)                                    // 0x59bbea
        capacity = 100;

    std::vector<PatchNode> pool(capacity);                  // Block, 0x59bbf0
    PatchHeap heap;                                         // 0x59bc11
    heap.slots.assign(capacity + 1, nullptr);
    int used = 0;                                           // n100_2

    // B. Seed (0x59bc7c): the packed seed, or a rejected-sampled water cell.
    int seedX;
    int seedY;
    if (seedXY != 0)                                        // 0x59bdec
    {
        seedX = static_cast<int16_t>(seedXY & 0xFFFF);
        seedY = static_cast<int16_t>(static_cast<uint32_t>(seedXY) >> 16);
    }
    else
    {
        for (int tries = 0;;)                               // 0x59bc85
        {
            unsigned int rx;
            do
            {
                rx = static_cast<unsigned int>(F2I64(
                    static_cast<double>(static_cast<uint32_t>(rng_.Next())) *
                    static_cast<double>(size_.mapWidth) * kUnitScale));
            }
            while (rx > static_cast<unsigned int>(size_.mapWidth - 1));
            unsigned int ry;
            do
            {
                ry = static_cast<unsigned int>(F2I64(
                    static_cast<double>(static_cast<uint32_t>(rng_.Next())) *
                    static_cast<double>(size_.mapHeight) * kUnitScale));
            }
            while (ry > static_cast<unsigned int>(size_.mapHeight - 1));

            seedX = static_cast<int>(ry) + static_cast<int>(rx) + 1;  // 0x59bcd8
            seedY = size_.mapWidth - static_cast<int>(rx)
                    + static_cast<int>(ry);                     // 0x59bce0
            ++tries;                                        // 0x59bcec
            if (tries >= 200)                               // 0x59bcf2
                return 0;

            if (workCells_[seedX + size_.workSide * seedY].data[14] == 0 &&
                IsWaterFamilyTile(CellAt(seedX, seedY)))    // 0x59bd1e
                break;
        }
    }

    // C. Aspect weights (0x59be46) - only consumed by the directional priority.
    float weightA = 0.0f;                                   // v86
    float weightB = 0.0f;                                   // v85
    if (bounds != nullptr)                                  // sentinel test 0x59be02
    {
        if (bounds->Width <= bounds->Height)                // 0x59be4a
        {
            weightB = 1.0f;
            weightA = static_cast<float>(
                static_cast<double>(bounds->Height) /
                static_cast<double>(bounds->Width) * 1.2);  // fidiv, 0x59be5a
        }
        else
        {
            weightA = 1.0f;
            weightB = static_cast<float>(
                static_cast<double>(bounds->Width) /
                static_cast<double>(bounds->Height) * 1.2);
        }
    }

    // Cursor starts on the seed (0x59bea7), in double precision.
    double cursorX = static_cast<double>(seedX);            // v95
    double cursorY = static_cast<double>(seedY);            // v96

    pool[0].coords.X = static_cast<int16_t>(seedX);         // 0x59bece
    pool[0].coords.Y = static_cast<int16_t>(seedY);
    pool[0].priority = 0.0f;
    workCells_[seedX + size_.workSide * seedY].data[15] = genCode_;  // 0x59bf0c
    used = 1;
    heap.Push(&pool[0], capacity);                          // 0x59bf24
    PatchNode* node = heap.Pop();                           // 0x59bf6b

    // Ellipse reciprocals handed to sub_59BAB0 (0x59bfab).
    const double halfWidth  = bounds ? bounds->Width * 0.5 : 0.0;
    const double halfHeight = bounds ? bounds->Height * 0.5 : 0.0;
    const double invHalfWidthSq  = bounds ? 1.0 / (halfWidth * halfWidth) : 0.0;
    const double invHalfHeightSq =
        bounds ? 1.0 / (halfHeight * halfHeight) : 0.0;

    // D. Growth loop (0x59bfe4). The seed cell runs one full round too, so the
    // cell count starts from it.
    int grown = 0;                                          // n400_2
    while (node != nullptr)                                 // 0x59bfe4
    {
        const int cx = node->coords.X;
        const int cy = node->coords.Y;
        workCells_[cx + size_.workSide * cy].data[14] = genCode_;  // 0x59c014
        CellAt(cx, cy)->IsoTileTypeIndex = 0;               // 0x59c02b

        for (int facing = 0; facing < 8; facing += 2)       // 0x59c04b
        {
            const int nx = cx + kPatchDirX[facing];
            const int ny = cy + kPatchDirY[facing];
            if (!CellExists(nx, ny))                        // 0x59c0a9
                continue;

            const int wi = nx + size_.workSide * ny;
            if (workCells_[wi].data[14] != 0 ||
                workCells_[wi].data[15] == genCode_)        // 0x59c0f0
                continue;

            MapCell* neighbour = CellAt(nx, ny);
            if (!IsWaterFamilyTile(neighbour))              // 0x59c10c
                continue;
            if (used >= capacity)                           // 0x59c11a
                continue;
            if (!PatchInBounds(nx | (ny << 16), bounds, boundsMode,
                               invHalfWidthSq, invHalfHeightSq))  // 0x59c158
                continue;

            const int curX = F2I64(cursorX + 0.5);          // 0x59c16b
            const int curY = F2I64(cursorY + 0.5);          // 0x59c19e
            double priority;
            if (priorityMode)                               // 0x59c1ab
            {
                priority = PatchDirectionalPriority(
                    curX | (curY << 16), nx | (ny << 16), bounds,
                    centerXY, weightA, weightB);
            }
            else                                            // 0x59c1d7
            {
                const int dx = nx - curX;
                const int dy = ny - curY;
                // YRMath::sqrt (0x4cac40) - the game's table sqrt, not libm's;
                // see MapGenFastSqrt.h for why the difference matters here.
                const double dist = static_cast<double>(MapGenFastSqrt::Sqrt(
                    static_cast<double>(dx * dx + dy * dy)));   // 0x59c1c9
                priority =
                    static_cast<double>(static_cast<uint32_t>(rng_.Next())) *
                    5.0 * kUnitScale + dist;                    // 0x59c1d7
            }

            PatchNode* pushed = &pool[used];                // 0x59c233
            pushed->coords.X = static_cast<int16_t>(nx);
            pushed->coords.Y = static_cast<int16_t>(ny);
            pushed->priority = static_cast<float>(priority);
            workCells_[wi].data[15] = genCode_;
            ++used;
            heap.Push(pushed, capacity);                    // 0x59c25a
        }

        cursorX += rng_.Gaussian() * step;                  // 0x59c2c1
        cursorY += rng_.Gaussian() * step;                  // 0x59c2d6
        ++grown;                                            // 0x59c2e7

        node = heap.Pop();                                  // 0x59c2f9
        if (grown >= area)                                  // 0x59c32c
            break;
    }

    // E. Drain the leftover queue (0x59c332): the queued frontier fringe turns
    // to land as well, so the patch does not end in half-cell zigzag. No RNG.
    for (PatchNode* left = heap.Pop(); left != nullptr; left = heap.Pop())
    {
        const int px = left->coords.X;
        const int py = left->coords.Y;
        const int wi = px + size_.workSide * py;
        if (workCells_[wi].data[14] == 0 &&
            IsWaterFamilyTile(CellAt(px, py)))              // 0x59c35e
        {
            CellAt(px, py)->IsoTileTypeIndex = 0;           // 0x59c36f
            workCells_[wi].data[14] = genCode_;
        }
        ++grown;                                            // 0x59c3c0
    }

    // F. Smoothing check, then the shore family cells are reclaimed as land
    // (0x59c43d / 0x59c449).
    const bool smoothed = SmoothWaterBody(genCode_, 1);     // 0x59c43d
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            if (cell->IsoTileTypeIndex >= shorePieces_ &&
                cell->IsoTileTypeIndex < shorePieces_ + 42)
            {
                cell->IsoTileTypeIndex = 0;                 // 0x59c46b
                cell->Height = 0;
            }
        }
    }

    // G. Success / rollback (0x59c486).
    if (smoothed)                                           // 0x59c51b
    {
        const int result = grown;
        ++genCode_;                                         // 0x59c51e
        return result;
    }

    CellIterator rollback;
    rollback.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = rollback.Next())
    {
        const int x = cell->MapCoords & 0xFFFF;
        const int y = static_cast<uint32_t>(cell->MapCoords) >> 16;
        WorkCell& w = workCells_[x + size_.workSide * y];
        if (w.data[14] == genCode_)                         // 0x59c4c0
        {
            w.data[14] = 0;                                 // 0x59c4d4
            w.Byte(75) = 0;                                 // 0x59c4dc
            cell->IsoTileTypeIndex = waterTileIndex_;       // 0x59c4e4
            cell->Height = 0;
            cell->Level = baseLevel_;                       // 0x59c4f0
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// PatchInBounds - sub_59BAB0, the patch's boundary test.
//
// The all-zero sentinel rect (nullptr here) accepts everything. Otherwise the
// mode selects one of two tests:
//
//   mode 0 (0x59bb87) - plain AABB: the cell's X / Y must fall inside
//     [rect.X, rect.X + rect.Width) and [rect.Y, rect.Y + rect.Height).
//
//   mode 1 (0x59bb00) - ellipse: the diamond coords are first rotated into
//     screen space, then the rect is centred on itself and the cell must satisfy
//     u^2 * (1/(W/2)^2) + v^2 * (1/(H/2)^2) < 1, where
//       u = (X - 2*rect.X - Y + Width' - 1) * 0.5 - rect.Width  * 0.5
//       v = (X - 2*rect.Y - Width' + Y - 1) * 0.5 - rect.Height * 0.5
//     and Width' is the map's diamond row width (MouseClass MapRect.Width).
// ---------------------------------------------------------------------------
bool RandomMapGenerator::PatchInBounds(int cellXY, const MapRectTag* bounds,
                                       int boundsMode,
                                       double invHalfWidthSq,
                                       double invHalfHeightSq) const
{
    if (bounds == nullptr)                                  // 0x59bae5 sentinel
        return true;

    const int cellX = static_cast<int16_t>(cellXY & 0xFFFF);
    const int cellY = static_cast<int16_t>(static_cast<uint32_t>(cellXY) >> 16);

    if (boundsMode)                                         // 0x59baf4 ellipse
    {
        const double u = (cellX - 2 * bounds->X - cellY
                          + size_.mapWidth - 1) * 0.5
                         - bounds->Width * 0.5;             // 0x59bb3f
        const double v = (cellX - 2 * bounds->Y - size_.mapWidth + cellY - 1)
                         * 0.5
                         - bounds->Height * 0.5;            // 0x59bb54
        return v * v * invHalfHeightSq + u * u * invHalfWidthSq < 1.0;
    }

    // 0x59bb87 plain AABB.
    return cellX >= bounds->X && cellX < bounds->X + bounds->Width &&
           cellY >= bounds->Y && cellY < bounds->Y + bounds->Height;
}

// ---------------------------------------------------------------------------
// PatchDirectionalPriority - sub_59B940, priority mode 1.
//
// Scores one candidate cell for the continent / team-continent generators. The
// region centre -> candidate vector is taken in screen space (u along (x-y)/2,
// v along (x+y)/2), normalised, weighted per axis and multiplied by the
// Chebyshev distance from the cursor to the candidate. Candidates that point
// along the region's long axis therefore score highest, which is what stretches
// the patch.
//
// With the all-zero sentinel rect (nullptr here) it degenerates to the plain
// Euclidean cursor -> candidate distance. No RNG.
// ---------------------------------------------------------------------------
double RandomMapGenerator::PatchDirectionalPriority(int cursorXY, int cellXY,
                                                    const MapRectTag* bounds,
                                                    int centreXY,
                                                    float weightA,
                                                    float weightB) const
{
    const int cursorX = static_cast<int16_t>(cursorXY & 0xFFFF);
    const int cursorY = static_cast<int16_t>(static_cast<uint32_t>(cursorXY) >> 16);
    const int cellX   = static_cast<int16_t>(cellXY & 0xFFFF);
    const int cellY   = static_cast<int16_t>(static_cast<uint32_t>(cellXY) >> 16);

    // 0x59b979 sentinel rect: plain Euclidean distance.
    if (bounds == nullptr)
    {
        const int dx = cursorX - cellX;                     // 0x59ba7b
        const int dy = cursorY - cellY;
        return static_cast<double>(MapGenFastSqrt::Sqrt(
            static_cast<double>(dx * dx + dy * dy)));        // 0x59ba94
    }

    const int centreX = static_cast<int16_t>(centreXY & 0xFFFF);
    const int centreY =
        static_cast<int16_t>(static_cast<uint32_t>(centreXY) >> 16);

    // Centre -> candidate, rotated into screen space (arithmetic shifts, so
    // negative coordinates floor toward -inf exactly like the x86 sar).
    const int du = std::abs(((cellX - cellY) >> 1) -
                            ((centreX - centreY) >> 1));     // v10, 0x59b9b1
    const int dv = std::abs(((cellX + cellY) >> 1) -
                            ((centreX + centreY) >> 1));     // v12, 0x59b9c4
    if (du == 0 && dv == 0)                                  // 0x59b9d4
        return 0.0;

    const double len = static_cast<double>(MapGenFastSqrt::Sqrt(
        static_cast<double>(du * du + dv * dv)));            // v14, 0x59b9fe

    int chebyshev = std::abs(cursorY - cellY);               // v15
    const int dxAbs = std::abs(cursorX - cellX);             // v21
    if (dxAbs > chebyshev)                                   // 0x59ba3e
        chebyshev = dxAbs;

    const double unitU = du / len;                           // v20, 0x59ba36
    const double unitV = dv / len;                           // v17, 0x59ba3a
    return (unitV * weightB + weightA * unitU) * chebyshev;  // 0x59ba58
}

// ---------------------------------------------------------------------------
// GenerateSpecialTerrain - sub_59C580 (LandType 3/4, Inland / Mountainous)
//
// Full annotated walkthrough: sub_59C580_注释.md (flowchart §11).
//
// Implemented: prologue (preparation, 0x59c58e - 0x59c59b) + river stage
//              dispatch loop (0x59c5ac - 0x59c5db) + lake stage
//              (0x59c5e8 - 0x59c60b, up to 10 GenerateLake tries).
// GenerateRiver / GenerateDelta / GenerateLake are all implemented, so this
// whole special-terrain path (sub_59C580) now runs end to end.
// ---------------------------------------------------------------------------
void RandomMapGenerator::GenerateSpecialTerrain()
{
    // [Preparation] generation-code increment (0x59c58e - 0x59c59b,
    // ++this[194] / ++genCode_). Every cell carved by this special-water
    // pass gets stamped with the new code (work-cell generation mark,
    // byte offset 56). On any failure the rollback pass identifies this
    // attempt's cells exactly by "generation mark == current code" and
    // clears them (rollback details: doc §5.9 / §6.7). The code is 0 on
    // entry: LandType 3/4 never runs sub_59A6C0 (which sets it to 1), so
    // after the increment the current code is 1, distinct from the
    // map-wide 0 (free land).
    ++genCode_;

    // ---- Stage 1: river (0x59c5ac - 0x59c5db) ----
    // Enabled only when the land type is Inland or Mountainous (this[15]
    // is 3 or 4; cached into a register at entry) and water amount > 20
    // (this[19]). Up to 10 attempts, first success stops (at most one
    // main river per map); on success the generation code is incremented
    // again, freeing a fresh code for the next water body (lakes).
    int landType = static_cast<int>(config_.landType);          // 0x59c594 mov eax,[esi+3Ch]
    DiagLog("SPEC g=%d landType=%d water=%d", genCode_, landType, waterAmount_);
    if ((landType == 3 || landType == 4) && waterAmount_ > 20)  // 0x59c598/0x59c5a3/0x59c5a8
    {
        for (int i = 0; i < 10; ++i)                            // 0x59c5b5 max 10 tries
        {
            // Zero the start before each attempt -> (0,0) = automatic
            // mode (random edge start)
            int startXY = 0;                                    // 0x59c5c1/0x59c5c6 v6=v7=0
            DiagLog("RIVER-TRY %d", i);
            if (GenerateRiver(&startXY, 0.0, false))            // 0x59c5cb top-level call
            {
                ++genCode_;                                     // 0x59c5db
                DiagLog("RIVER-OK g=%d", genCode_);
                break;                                          // 0x59c5db
            }
            // [SNAPSHOT-OFF] SaveStageSnapshot("GenerateRiver");
        }
    }
    // ---- Stage 2: independent lakes (0x59c5e8 - 0x59c60b) ----
    // NOT gated by land type or water amount: the lake body checks its own
    // budget against the used-water counter inside sub_59C920 (§6.1). Up to 10
    // attempts, each with seed (0,0) so the callee samples a point itself; the
    // first success bumps the generation code (isolating a later rollback) and
    // ends the stage. Vanilla's sub_59C580 returns 1 on a successful lake and 0
    // when all ten fail - the caller here ignores it, so this stays void.
    //
    // The stage itself consumes no RNG: seed selection and the rest happen
    // inside GenerateLake.
    for (int i = 0; i < 10; ++i)                                // 0x59c5e8 max 10 tries
    {
        int lakeSeed = 0;                                       // 0x59c5f5 (0,0) = automatic
        DiagLog("LAKE-TRY %d", i);
        if (GenerateLake(&lakeSeed))                            // 0x59c5fb
        {
            // [SNAPSHOT-OFF] SaveStageSnapshot("GenerateLake");
            ++genCode_;                                         // 0x59c605
            DiagLog("LAKE-OK g=%d", genCode_);
            return;                                             // 0x59c60b return 1
        }
    }
}

// ---------------------------------------------------------------------------
// Tile predicates - sub_485060 / sub_486380
//
//   IsWaterTile       sub_485060: IsoTileTypeIndex in [nIdx, nIdx + 14)
//   IsPlaceholderTile sub_486380: IsoTileTypeIndex is 0 or 0xFFFF
//
// nIdx is the WaterSet base tile (game global 0xAA0738 = waterTileIndex_).
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsWaterTile(const MapCell* cell) const
{
    const int tile = cell->IsoTileTypeIndex;
    return tile >= waterTileIndex_ && tile < waterTileIndex_ + 14;
}

bool RandomMapGenerator::IsPlaceholderTile(const MapCell* cell) const
{
    const int tile = cell->IsoTileTypeIndex;
    return tile == 0 || tile == 0xFFFF;
}

// ---------------------------------------------------------------------------
// CellAt - MapClass::GetCellAt_MapCrd (0x5657AF).
//
// The slot index is X + (Y * slotStride_); out-of-range indices and null slots fall
// back to MapClass::InvalidCell with MapCoords stamped with the requested
// coords (here invalidCell_). Every in-diamond coordinate resolves to a real
// cell, so the sentinel is only ever produced for coordinates outside the
// diamond.
// ---------------------------------------------------------------------------
MapCell* RandomMapGenerator::CellAt(int x, int y)
{
    const int index = x + y * slotStride_;
    if (index >= 0 && index < slotRows_ * slotStride_ && cellSlots_[index])
        return cellSlots_[index];

    invalidCell_.MapCoords = (static_cast<int>(static_cast<int16_t>(x)) & 0xFFFF)
                           | (static_cast<int>(static_cast<int16_t>(y)) << 16);
    return &invalidCell_;
}

// ---------------------------------------------------------------------------
// RawSlot - Cells.Items[index] as read by the TileNeighbourMask scan.
//
// Unlike CellAt there is no InvalidCell fallback: an out-of-range index or a
// null slot yields nullptr, which the mask scan reads as "no neighbour".
// ---------------------------------------------------------------------------
MapCell* RandomMapGenerator::RawSlot(int index)
{
    if (index < 0 || index >= slotRows_ * slotStride_)
        return nullptr;
    return cellSlots_[index];
}

// ---------------------------------------------------------------------------
// GetNeighbourCell - CellClass::GetNeighbourCell (0x481810).
//
// Adds Neighbours[facing & 7] to the cell's MapCoords (16-bit adds, matching
// the vanilla CellStruct arithmetic) and looks the result up with CellAt.
//   [0] (0,-1)  [1] (1,-1)  [2] (1,0)  [3] (1,1)
//   [4] (0, 1)  [5] (-1,1)  [6] (-1,0) [7] (-1,-1)
// ---------------------------------------------------------------------------
MapCell* RandomMapGenerator::GetNeighbourCell(const MapCell* cell, int facing)
{
    static const int16_t kDirX[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
    static const int16_t kDirY[8] = {-1, -1,  0,  1,  1,  1,  0, -1 };

    const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF) + kDirX[facing & 7];
    const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16) + kDirY[facing & 7];
    return CellAt(x, y);
}

// ============================================================================
// RMGSettings implementation
// ============================================================================

// ---------------------------------------------------------------------------
// SetDefaults - vanilla RA2 YR RMGMD.INI contents
// (vanilla always ships the file; standalone build uses it as fallback)
// ---------------------------------------------------------------------------
void RMGSettings::SetDefaults()
{
    // The values the RMG instance's own constructor installs (0x59585f /
    // 0x595873), i.e. what the vanilla INI reader passes as the ReadInteger
    // default. The shipped rmgmd.ini then overwrites them with 900 / 1050.
    MinTiberium = 2500;   // RMGMinimumTiberium
    MaxTiberium = 5500;   // RMGMaximumTiberium
    MaxTrees    = 600;    // MaxTrees

    // by time of day: (morning, day, dusk, night)
    LevelLightSettings    = { 3,   3,   3,   3   };

    // by time of day: range 0 (black) to 100 (normal brightness)
    TemperateAmbientLight = { 75,  100, 75,  35  };
    SnowAmbientLight      = { 75,  100, 75,  55  };
    TemperateAmbientRed   = { 109, 108, 71,  51  };
    TemperateAmbientGreen = { 80,  94,  81,  46  };
    TemperateAmbientBlue  = { 49,  68,  119, 141 };
    SnowAmbientRed        = { 112, 99,  106, 76  };
    SnowAmbientGreen      = { 80,  104, 107, 74  };
    SnowAmbientBlue       = { 133, 107, 154, 118 };

    // by land type: (archipelago, continent, team continent, inland, mountainous)
    VegetationMinimums    = { 60,  60,  60,  60,  60  };
    VegetationMaximums    = { 100, 100, 100, 100, 100 };

    // by time of day: valid building INI names
    TemperateOrePatchLamps = { "TEMMORLAMP", "TEMDAYLAMP", "TEMDUSLAMP", "TEMNITLAMP" };
    SnowOrePatchLamps      = { "SNOMORLAMP", "SNODAYLAMP", "SNODUSLAMP", "SNONITLAMP" };
}

// Safe index helpers (empty group -> 0, out-of-range -> last element)
static int ListAt(const std::vector<int>& v, int i)
{
    if (v.empty()) return 0;
    if (i < 0) i = 0;
    if (i >= (int)v.size()) i = (int)v.size() - 1;
    return v[i];
}

int RMGSettings::LevelLight(int timeOfDay) const
{
    return ListAt(LevelLightSettings, timeOfDay);
}

int RMGSettings::AmbientLight(int theater, int timeOfDay) const
{
    const std::vector<int>& v = theater ? SnowAmbientLight : TemperateAmbientLight;
    return ListAt(v, timeOfDay);
}

int RMGSettings::VegetationMin(int landType) const
{
    return ListAt(VegetationMinimums, landType);
}

int RMGSettings::VegetationMax(int landType) const
{
    return ListAt(VegetationMaximums, landType);
}

// ============================================================================
// sub_5981F0 replication - re-initialize map data from RMGMD.INI
// ============================================================================

// ---------------------------------------------------------------------------
// ReadIntList - comma-separated int list (replicates sub_475D70)
//   - 512-byte read buffer
//   - strtok by "," then atoi per token
//   - empty token terminates the loop (vanilla: "if (!*token) break")
//   - key absent -> list stays empty (vanilla "not read" branch)
// ---------------------------------------------------------------------------
static void ReadIntList(const char* path, const char* key, std::vector<int>& out)
{
    out.clear();
    char buffer[512];
    DWORD n = RmReadString("General", key, "", buffer, 512, path);
    if (n == 0)
        return;

    char* next = nullptr;
    for (char* tok = strtok_s(buffer, ",", &next); tok; tok = strtok_s(nullptr, ",", &next))
    {
        if (!*tok)
            break;
        out.push_back(atoi(tok));
    }
}

// ---------------------------------------------------------------------------
// ReadNameList - comma-separated building name list (OrePatchLamps part of
// sub_5981F0)
//   - 128-byte read buffer (vanilla pBuffer_[128])
//   - list is cleared in both branches (read / not read)
//   - vanilla resolves each token via BuildingTypeClass::Find and skips
//     unknown names; standalone build keeps the raw names
// ---------------------------------------------------------------------------
static void ReadNameList(const char* path, const char* key, std::vector<std::string>& out)
{
    out.clear();
    char buffer[128];
    DWORD n = RmReadString("General", key, "", buffer, 128, path);
    if (n == 0)
        return;

    char* next = nullptr;
    for (char* tok = strtok_s(buffer, ",", &next); tok; tok = strtok_s(nullptr, ",", &next))
    {
        if (!*tok)
            break;
        out.push_back(tok);
    }
}

// ---------------------------------------------------------------------------
// ReInitMapData (replicates sub_5981F0, called from sub_599650 at 0x599685)
//
// Vanilla flow:
//   1. Open RMGMD.INI from the game directory (CCFileClass + CCINIClass)
//   2. Unconditionally clear the setting groups
//   3. Read [General] keys in this exact order:
//        RMGMinimumTiberium / RMGMaximumTiberium  (int, default = current value)
//        RMGLevelLightSettings, RMGVegetationMinimums, RMGVegetationMaximums,
//        TemperateAmbientLight, SnowAmbientLight, TemperateAmbientRed,
//        TemperateAmbientGreen, TemperateAmbientBlue, SnowAmbientRed,
//        SnowAmbientGreen, SnowAmbientBlue   (comma-separated int lists)
//        MaxTrees   (int, default = current value)
//        TemperateOrePatchLamps / SnowOrePatchLamps  (name lists)
//   4. Destroy the INI object and close the file
//
// Differences for the standalone build:
//   - INI parsing uses Win32 GetPrivateProfile* instead of CCINIClass
//   - Vanilla always ships RMGMD.INI next to the exe; when the file is
//     missing here the built-in vanilla defaults are kept instead
// ---------------------------------------------------------------------------
bool RandomMapGenerator::ReInitMapData()
{
    // [FA2sp] No RMGMD.INI: the host dialog owns these three groups
    // (RMGMinimumTiberium / RMGMaximumTiberium / MaxTrees / vegetation range).
    // Everything else in RMGSettings only fed SaveMapFile's [Lighting] block,
    // which the editor integration does not write.
    //
    // Do NOT re-seed the defaults here: the constructor already did, and the
    // host calls SetSettings() BEFORE GenerateMapBody(), so resetting would
    // silently drop whatever the dialog collected.
    if (!s_bLoadRmgmdIni)
    {
        iniLoaded_ = false;
        return false;
    }

    // RMGMD.INI next to the executable (vanilla: game directory)
    wchar_t wdir[MAX_PATH];
    GetModuleFileNameW(nullptr, wdir, MAX_PATH);
    wchar_t* slash = wcsrchr(wdir, L'\\');
    if (slash)
        slash[1] = L'\0';
    wchar_t wpath[MAX_PATH];
    swprintf_s(wpath, L"%sRMGMD.INI", wdir);

    if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES)
    {
        // File missing: keep built-in defaults (vanilla never hits this path)
        iniLoaded_ = false;
        return false;
    }

    char path[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);

    settings_.LevelLightSettings.clear();
    settings_.TemperateAmbientLight.clear();
    settings_.SnowAmbientLight.clear();
    settings_.TemperateAmbientRed.clear();
    settings_.TemperateAmbientGreen.clear();
    settings_.TemperateAmbientBlue.clear();
    settings_.SnowAmbientRed.clear();
    settings_.SnowAmbientGreen.clear();
    settings_.SnowAmbientBlue.clear();
    settings_.VegetationMinimums.clear();
    settings_.VegetationMaximums.clear();
    settings_.TemperateOrePatchLamps.clear();
    settings_.SnowOrePatchLamps.clear();

    settings_.MinTiberium = (int)RmReadInt(
        "General", "RMGMinimumTiberium", (UINT)settings_.MinTiberium, path);
    settings_.MaxTiberium = (int)RmReadInt(
        "General", "RMGMaximumTiberium", (UINT)settings_.MaxTiberium, path);

    ReadIntList(path, "RMGLevelLightSettings",    settings_.LevelLightSettings);
    ReadIntList(path, "RMGVegetationMinimums",    settings_.VegetationMinimums);
    ReadIntList(path, "RMGVegetationMaximums",    settings_.VegetationMaximums);
    ReadIntList(path, "TemperateAmbientLight",    settings_.TemperateAmbientLight);
    ReadIntList(path, "SnowAmbientLight",         settings_.SnowAmbientLight);
    ReadIntList(path, "TemperateAmbientRed",      settings_.TemperateAmbientRed);
    ReadIntList(path, "TemperateAmbientGreen",    settings_.TemperateAmbientGreen);
    ReadIntList(path, "TemperateAmbientBlue",     settings_.TemperateAmbientBlue);
    ReadIntList(path, "SnowAmbientRed",           settings_.SnowAmbientRed);
    ReadIntList(path, "SnowAmbientGreen",         settings_.SnowAmbientGreen);
    ReadIntList(path, "SnowAmbientBlue",          settings_.SnowAmbientBlue);

    settings_.MaxTrees = (int)RmReadInt(
        "General", "MaxTrees", (UINT)settings_.MaxTrees, path);

    ReadNameList(path, "TemperateOrePatchLamps", settings_.TemperateOrePatchLamps);
    ReadNameList(path, "SnowOrePatchLamps",      settings_.SnowOrePatchLamps);

    iniLoaded_ = true;
    return true;
}
