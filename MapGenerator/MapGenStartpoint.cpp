// ============================================================================
// MapGenStartpoint.cpp - the "RMG: Creating starting points" stage
// (sub_598960 @ 0x598e9e - 0x598ebe) and its two steps.
//
// The stage body is a retry loop: it keeps calling its two steps until both
// answer "success".
//
//     0x598e9e  push "RMG: Creating starting points\n", nullsub_1   debug
//     0x598ea8  add esp, 4
//     0x598eab  sub_594B50()       <- step 1, no arguments, answers in AL
//     0x598eb0  cmp al, bl         ; bl is 0 here
//     0x598eb2  jz  0x598eab       ; 0 -> run both steps again
//     0x598eb4  mov ecx, esi       ; this
//     0x598eb6  sub_5A1FB0(this)   <- step 2, answers in AL
//     0x598ebb  cmp al, bl
//     0x598ebd  jz  0x598eab       ; 0 -> run both steps again
//     0x598ebf  push "RMG: Adding tech buildings\n", ...            next stage
//
// A failed run leaves whatever state the failed step produced; the retry does
// not roll anything back. Both steps always answer 1 in practice, so the loop
// runs once.
//
// Decompile: 598E9E_CreatingStartingPoints.c in the project decompile folder.
// ============================================================================

#include "pch.h"
#include "MapGen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>

// ---------------------------------------------------------------------------
// The two region-size values the floor formula reads are NOT constants - they
// are RMG-instance fields (full note in MapGenMakingSub.cpp):
//
//   dword_ABE15C = this[97] = the map HEIGHT (size_.height)
//   dword_ABE158 = this[96] = the map WIDTH  (size_.width)
//
// sub_597A30 loads the preset INI's [RandomMap] Height / Width into this[26] /
// this[25] (0x597b3d / 0x597a24) and sub_599650 interpolates them into
// this[96] / this[97] (0x599700 / 0x599748). Both land in 70..120, so the 400
// cell floor only bites on the smaller sizes: 70*70*0.03 = 147 -> 400, while
// 120*120*0.03 = 432 stays 432.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The starting-point budget is also an instance field: this[20] = the player
// count (this[20] +0x50 = dword_ABE028 = 0xABE028).
//
// sub_596C70 reads it straight off the dialog's player TRACKBAR - 0x596dd3
// GetDlgItem(hDlg, 1003) followed by SendMessageA(hWnd, TBM_GETPOS = 0x400) -
// and sub_5975E0 clamps it to 2..8; the preset INI carries it as [RandomMap]
// NumPlayers (0x597b5c). An earlier reading of this file held it to the constant
// 4, because two OTHER call paths write 4 first (0x7a2494 in sub_7A2330, the
// "RandMap.Sed" scenario build, and 0x596018 in sub_595FB0, the dialog's
// file-load branch). Those run BEFORE the trackbar handler, so the value the
// generator actually sees is the trackbar position. The port passes the player
// count through MapGenConfig::playerCount.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// dword_ABE030 = this[22] = the rolled TiberiumLayout option (0..100). It is the
// one option field read with ABSOLUTE addressing during generation
// (0x594f49 "fild dword_ABE030" inside sub_594F40), which is why xrefs_to does
// see this read; the preset INI carries it as [RandomMap] TiberiumLayout
// (sub_597A30 @0x597c75). The port reads the rolled globalOptions_.tiberiumLayout.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The scenario's [Map] LocalSize = {2, 5, W, H}, which MapClass::VisibleRect
// holds. MapGenMakingSub.cpp carries the same pair for IsWithinUsableArea;
// W and H are size_.width / size_.height.
// ---------------------------------------------------------------------------
static const int kVisibleOffsetX = 2;
static const int kVisibleOffsetY = 5;

// ---------------------------------------------------------------------------
// MapClass::GetMoveError (0x56d230 - 0x56d399).
//
//   idx = coords.X + coords.Y * (MapRect.Height + MapRect.Width + 1)
//   if (idx < 0)                     idx = 0
//   if (idx >= ValidMapCellCount)    idx = ValidMapCellCount - 1
//   return MovementZones[zone][ LevelAndPassability[idx].ZoneArrayIndex ]
//
// The vanilla first runs a bridge branch when its fourth argument is true; the
// generator always passes false, so it is not modelled (it only rewrites the
// coordinate to the bridge's far end).
// ---------------------------------------------------------------------------
int RandomMapGenerator::GetMoveError(CellStruct coords, int movementZone) const
{
    // The vanilla indexes MapClass::MovementZones[nMovementZone] with no test at
    // all; that array holds thirteen tables, so the port only guards the ends.
    if (movementZone < 0 || movementZone >= 13)
        return 0;

    int idx = coords.X + coords.Y * (size_.height + size_.width + 1);
    if (idx < 0)
        idx = 0;
    else if (idx >= static_cast<int>(levelAndPassability_.size()))
        idx = static_cast<int>(levelAndPassability_.size()) - 1;

    const int zoneIndex = levelAndPassability_[static_cast<size_t>(idx)].zoneArrayIndex;
    const std::vector<int>& table = movementZones_[movementZone];
    if (zoneIndex < 0 || zoneIndex >= static_cast<int>(table.size()))
        return 0;
    return table[static_cast<size_t>(zoneIndex)];
}

// ---------------------------------------------------------------------------
// sub_56C510 (0x56c510 - 0x56cb8d) - the movement-zone rebuild.
//
// It tears the thirteen MovementZones tables down, flood-fills a zone number
// into every cell through sub_56CB90 while recording the zone borders in
// MapClass::ZoneConnections, then turns each of the thirteen rows into a
// move-error table (fill + merge the impassable zones into components). It
// answers the number of the zone with the LARGEST run - sub_594B50 reads that
// zone's slot as the reference move error every cell of a region must
// reproduce, and reads each cell's own slot through GetMoveError.
// ---------------------------------------------------------------------------
namespace
{
    // The static table at 0x82A594: thirteen rows of eight ints (read out of the
    // image). Row = the movement zone's index, column = a cell's passability
    // class. The value says how that class fares against a zone of this kind:
    // 1 is "cannot cross", 2 and 3 are the two grades that can. sub_56C510 only
    // ever tests "== 1", and the merge pass below uses the 1s as the set of
    // zones to group.
    const uint8_t kZonePassability[13][8] =
    {
        { 1, 2, 2, 2, 2, 2, 2, 3 },
        { 1, 1, 2, 2, 2, 2, 2, 3 },
        { 1, 1, 1, 2, 2, 2, 2, 3 },
        { 1, 1, 1, 1, 1, 1, 2, 3 },
        { 1, 1, 2, 1, 1, 2, 2, 3 },
        { 1, 2, 2, 1, 1, 2, 2, 3 },
        { 1, 1, 1, 2, 2, 2, 1, 3 },
        { 1, 2, 2, 2, 2, 1, 2, 3 },
        { 1, 1, 1, 2, 2, 1, 2, 3 },
        { 1, 1, 1, 1, 1, 1, 1, 3 },
        { 2, 2, 2, 2, 1, 2, 2, 3 },
        { 2, 2, 2, 1, 1, 2, 2, 3 },
        { 1, 1, 1, 2, 2, 2, 2, 3 },
    };

    // -----------------------------------------------------------------------
    // One scratch cell of MapClass::LevelAndPassability - four bytes, laid out
    // exactly as CellLevelPassabilityStruct (YRpp MapClass.h):
    //
    //     byte 0     the passability class of the cell that seeded the zone
    //     byte 1     the cell's level
    //     word 2     the zone number (ZoneArrayIndex)
    //
    // The zone number is ALWAYS the word at +2; byte +1 keeps the level and the
    // fill never writes it. The decompiles render those word accesses as byte
    // reads/writes at +1, so the assembly is what settles it:
    //
    //     sub_56CB90  0x56cbd6  mov [ebp+2], cx            write the zone
    //                 0x56cbef  mov di, [ebp+2]            read a border's zone
    //                 0x56cbc0  mov al, [ebp+1]            read the level
    //     sub_56C510  0x56c5ac  mov word ptr [eax+2], 0    clear a zone
    //                 0x56c621  mov al, [esi]              read the class byte
    //                 0x56c630  cmp word ptr [esi+2], 0    "already zoned" test
    // -----------------------------------------------------------------------
    inline int ZoneOf(const uint8_t* cell)
    {
        return cell[2] | (cell[3] << 8);
    }

    inline void SetZone(uint8_t* cell, int zone)
    {
        cell[2] = static_cast<uint8_t>(zone & 0xFF);
        cell[3] = static_cast<uint8_t>((zone >> 8) & 0xFF);
    }

    // -----------------------------------------------------------------------
    // Step 2's flood (sub_5A1FB0): an 8-byte node - the cell plus its
    // straight-line distance to the starting point - and a 1-based min-heap
    // over POINTERS to those nodes.
    //
    // The heap is the same vanilla object the lake stage ports as LakeHeap
    // (MapGenLake.cpp): 0x14 bytes { count, capacity 800, slots, maxSeen,
    // minSeen }, sift-down sub_5AD870, pop sub_5AC960, and an inline sift-up
    // that is skipped once count + 1 reaches the capacity. It is duplicated
    // here because that one is file-static. The two "seen" slots are dead
    // stores and are dropped.
    // -----------------------------------------------------------------------
    struct StartFloodNode
    {
        CellStruct coords;      // +0
        float      distance;    // +4
    };

    struct StartFloodHeap
    {
        static const size_t kCapacity = 800;   // vanilla: `if (count + 1 < cap)`

        std::vector<StartFloodNode*> slots;    // slots[0] unused
        size_t count;

        StartFloodHeap() : slots(kCapacity + 1, nullptr), count(0) {}

        // Sift-down (sub_5AD870) - the lake stage's copy, over this node type.
        void SiftDown(size_t a2)
        {
            size_t v2 = a2;
            size_t v3 = 2 * a2;
            const size_t v4 = 2 * a2 + 1;
            if (!(2 * a2 <= count)
                || slots[a2]->distance <= slots[2 * a2]->distance)
                v3 = a2;
            if (v4 <= count && slots[v3]->distance > slots[v4]->distance)
                v3 = 2 * a2 + 1;
            if (v3 == a2)
                return;
            do
            {
                const size_t v6 = 2 * v3 + 1;
                StartFloodNode* v7 = slots[v2];
                slots[v2] = slots[v3];
                v2 = v3;
                slots[v3] = v7;
                if (2 * v3 <= count
                    && slots[v3]->distance > slots[2 * v3]->distance)
                    v3 *= 2;
                if (v6 <= count && slots[v3]->distance > slots[v6]->distance)
                    v3 = v6;
            }
            while (v3 != v2);
        }

        // Sift-up insert. Silently dropped once count + 1 would reach the
        // capacity - the node keeps its mark but never enters the queue.
        void Push(StartFloodNode* node)
        {
            size_t v = count + 1;
            size_t parent = v >> 1;
            if (v >= kCapacity)
                return;
            while (v > 1 && slots[parent]->distance > node->distance)
            {
                slots[v] = slots[parent];
                v = parent;
                parent >>= 1;
            }
            slots[v] = node;
            count = v;
        }

        // Pop the nearest node (sub_5AC960): root out, last slot into its
        // place, shrink, sift down.
        StartFloodNode* Pop()
        {
            if (count == 0)
                return nullptr;
            StartFloodNode* root = slots[1];
            slots[1] = slots[count];
            slots[count] = nullptr;
            --count;
            SiftDown(1);
            return root;
        }
    };
}

int RandomMapGenerator::RebuildMoveZones()
{
    const int count = static_cast<int>(levelAndPassability_.size());
    if (count == 0)
        return 0;

    // The walk runs on the array's own bytes: four per cell, byte 0 holding the
    // passability class (which the fill reads but never writes) and the zone
    // number living in the 16-bit field at +2 (see ZoneOf / SetZone).
    uint8_t* cells = reinterpret_cast<uint8_t*>(levelAndPassability_.data());

    // ---- 1. clear (0x56c522 - 0x56c5c8) -----------------------------------
    for (int i = 0; i < 256; ++i)
        zoneConnections_[i].clear();
    for (int i = 0; i < count; ++i)
        SetZone(cells + 4 * i, 0);            // 0x56c5ac, word at +2
    lastRecordedZone_ = 0;

    // The passability values are needed again at the tail, so keep a copy (the
    // vanilla fills a VectorClass<PassabilityType> here).
    passabilityCopy_.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        passabilityCopy_[static_cast<size_t>(i)] = cells[4 * i];

    // ---- 2. stamp the zone numbers (0x56c5d0 - 0x56c6a4) ------------------
    // Zones count up from 1; the run length FillZoneFromRow returns is how far
    // to skip. A cell is "done" when its passability is the sentinel 7 or when
    // its zone field is already set - the vanilla's two-part test.
    int zone = 1;
    int largestZone = 0;
    int largestRun = -1;
    for (int i = 0; i < count; )
    {
        const bool done = (cells[4 * i] == 7 || ZoneOf(cells + 4 * i) != 0);
        if (done)
        {
            ++i;
            continue;
        }

        // The seed cell's passability is this zone's own passability; the flood
        // leaves byte 0 alone, so read it here (vanilla pushes it into its
        // PassabilityType vector at this point).
        zonePassabilityValue_.resize(static_cast<size_t>(zone) + 1);
        zonePassabilityValue_[static_cast<size_t>(zone)] = cells[4 * i];

        lastRecordedZone_ = 0;
        int covered = 0;
        const int run = FillZoneFromRow(cells + 4 * i, zone, &covered);

        if (run > largestRun)
        {
            largestRun = run;
            largestZone = zone;
        }
        ++zone;
        i += covered;
    }

    // ---- 3. the recorded borders ------------------------------------------
    // Vanilla also folds in MapClass::SubzoneTracking here (the loop over
    // this[24] entries at 0x56c716 - 0x56c8a0, which re-reads the tracking
    // records and adds the pairs they imply). The generator never creates
    // subzone tracking records, so that pass has nothing to add and is not
    // modelled.

    // ---- 4. the thirteen move-error tables (0x56c8b0 - 0x56cfd0) ----------
    // Each of the thirteen rows is built in two steps.
    //
    // 4a. Fill it from the static table at 0x82A594 (13 rows of 8 ints, read
    //     out of the image): 1 means "impassable for this passability class",
    //     anything else means walkable. So the slot starts as 1 = walkable,
    //     0 = impassable.
    //
    // 4b. Merge the impassable zones (0x56c850 - 0x56cfd0). The vanilla walks
    //     the row from zone 0 upward; every slot still holding 0 starts a new
    //     component, which is then flood-filled over the zone adjacency graph
    //     recorded in MapClass::ZoneConnections (this+0x14, the table sub_56CB90
    //     writes through RecordZonePair) and given the next component number,
    //     counting from 2. Walkable zones keep 1, and slot 0 is overwritten with
    //     -1 at the end of the row.
    //     Both of the vanilla's neighbour tests - "same static value" and
    //     "still unvisited" - are the same test: only zones whose static value
    //     is 1 are ever in a component.
    //
    //     The upshot is that the slot is not a flag but a move-error class:
    //     GetMoveError answers the class, and callers only ever compare classes
    //     for equality.
    //
    //     (Index 0 is a dummy - the vanilla seeds it with passability 7, whose
    //     column is 3 in all thirteen rows, so it never starts a component; and
    //     it is overwritten with -1 anyway. The port skips it - same result.)
    std::vector<std::vector<int> > adjacency(static_cast<size_t>(zone));
    for (int key = 0; key < 256; ++key)
    {
        const std::vector<int>& pairs = zoneConnections_[key];
        for (size_t i = 0; i < pairs.size(); ++i)
        {
            const int lo = pairs[i] & 0xFFFF;
            const int hi = (pairs[i] >> 16) & 0xFFFF;
            if (lo <= 0 || lo >= zone || hi <= 0 || hi >= zone)
                continue;
            adjacency[static_cast<size_t>(lo)].push_back(hi);
            adjacency[static_cast<size_t>(hi)].push_back(lo);
        }
    }

    for (int t = 0; t < 13; ++t)
    {
        std::vector<int>& table = movementZones_[t];
        table.assign(static_cast<size_t>(zone), 0);
        for (int z = 1; z < zone; ++z)
        {
            const int passability =
                static_cast<int>(zonePassabilityValue_[static_cast<size_t>(z)]);
            table[static_cast<size_t>(z)] =
                (kZonePassability[t][passability & 7] != 1) ? 1 : 0;
        }

        int group = 2;
        std::vector<int> stack;
        for (int z = 1; z < zone; ++z)
        {
            if (table[static_cast<size_t>(z)] != 0)
                continue;

            stack.clear();
            stack.push_back(z);
            table[static_cast<size_t>(z)] = group;
            while (!stack.empty())
            {
                const int current = stack.back();
                stack.pop_back();
                const std::vector<int>& neighbours =
                    adjacency[static_cast<size_t>(current)];
                for (size_t i = 0; i < neighbours.size(); ++i)
                {
                    const int n = neighbours[i];
                    if (table[static_cast<size_t>(n)] != 0)
                        continue;
                    table[static_cast<size_t>(n)] = group;
                    stack.push_back(n);
                }
            }
            ++group;
        }

        table[0] = -1;
    }

    // ---- 5. the ZoneArrayIndex is already in place ------------------------
    // The fill writes each cell's zone number straight into the 16-bit fields at
    // +2 (SetZone), so there is nothing to copy back - the vanilla has no such
    // step either.

    return largestZone;
}

// ---------------------------------------------------------------------------
// RecordZonePair - one push into zoneConnections_, i.e. the body of the four
// identical recording blocks inside sub_56CB90.
//
//   key = (zone & 0xF) | (16 * (other & 0xF))
//   the vanilla walks the entry's pair list and writes only when absent, so the
//   table holds unique pairs
//
// Both zone numbers are packed into one byte there, which is why a zone number
// is limited to four bits. The entry is a VectorClass<unsigned long> that keeps
// each packed value twice; the port keeps one copy (see MapGen.h).
// ---------------------------------------------------------------------------
void RandomMapGenerator::RecordZonePair(int zone, int other)
{
    const int key = (zone & 0xF) | (16 * (other & 0xF));
    const int value = zone | (other << 16);

    std::vector<int>& table = zoneConnections_[key];
    for (size_t i = 0; i < table.size(); ++i)
    {
        if (table[i] == value)
            return;
    }
    table.push_back(value);
}

// ---------------------------------------------------------------------------
// sub_56CB90 (0x56cb90 - 0x56cfff) - the recursive scanline fill.
//
// `row` points at one row of the map's LevelAndPassability array, where every
// cell takes four bytes (see ZoneOf / SetZone):
//
//     cell[0]   the passability class of the cell that seeded this zone
//               (never written by the fill; the value 6 marks a wildcard level)
//     cell[1]   the cell's level, used as the "same layer" test
//     cell[2..3] the zone number, 0 while unzoned
//
// The run this call owns is bounded by `west` and `east`; every cell in it is
// stamped with `zone`, and every border with a different zone is recorded in
// zoneConnections_ through RecordZonePair. The rows above and below the run are
// then walked and the stretches that still carry our class byte recurse.
//
// Two details worth keeping:
//   - the westward grow tests |level difference| >= 2, the eastward grow >= 4,
//     so the east walk stops far sooner than the west one;
//   - a row whose class byte is 6 treats any level as connected (vanilla's v74),
//     which makes level 6 a wildcard layer.
// ---------------------------------------------------------------------------
int RandomMapGenerator::FillZoneFromRow(uint8_t* row, int zone, int* out)
{
    const int startZone = row[0];
    const bool wildcardLevel = (startZone == 6);
    int level = row[1];

    // ---- grow west (0x56cbb0) ---------------------------------------------
    uint8_t* west = row;
    for (;;)
    {
        const int diff = west[1] - level;
        if (diff >= 2 || diff <= -2)
            break;
        SetZone(west, zone);
        level = west[1];
        const int prevZone = west[-4];
        west -= 4;
        if (prevZone != startZone)
            break;
    }

    int westLevel = level;

    // ---- record the west border (0x56cbe0) --------------------------------
    {
        const int borderZone = ZoneOf(west);
        if (borderZone != 0)
        {
            const int diff = west[1] - westLevel;
            if (((diff < 2 && diff > -2) || wildcardLevel)
                && borderZone != lastRecordedZone_ && borderZone != zone)
            {
                RecordZonePair(zone, borderZone);
                lastRecordedZone_ = borderZone;
            }
        }
    }

    // ---- grow east (0x56cc10) ---------------------------------------------
    uint8_t* east = row;
    if (row[0] == startZone)
    {
        int eastLevel = westLevel;
        do
        {
            const int diff = east[1] - eastLevel;
            if (diff >= 4 || diff <= -4)
                break;
            SetZone(east, zone);
            eastLevel = east[1];
            east += 4;
            westLevel = eastLevel;
        }
        while (east[0] == startZone);
    }

    // ---- record the east border -------------------------------------------
    {
        const int borderZone = ZoneOf(east);
        if (borderZone != 0)
        {
            const int diff = east[1] - westLevel;
            if (((diff < 2 && diff > -2) || wildcardLevel)
                && borderZone != lastRecordedZone_ && borderZone != zone)
            {
                RecordZonePair(zone, borderZone);
                lastRecordedZone_ = borderZone;
            }
        }
    }

    // ---- run extent and the two neighbouring rows -------------------------
    int runStart = ((int)(east - west) >> 2) - 1;              // v77
    *out = ((int)(east - row) >> 2) - 1;                       // a4

    uint8_t* aboveWalk = west + 4;                             // v37
    uint8_t* belowEnd = east - 4;                              // v38

    const int rowSpan = (size_.height + size_.width + 1) * 4;  // v39 * 4
    const int upStep = rowSpan + 4;                            // v40

    uint8_t* aboveStart = aboveWalk - upStep;                  // v41
    uint8_t* belowStart = aboveWalk + rowSpan - 4;             // v85
    uint8_t* aboveEnd = belowEnd + upStep;                     // v84
    uint8_t* belowStop = belowEnd - rowSpan + 4;               // v87

    int aboveOut = 0;
    int belowOut = 0;

    // ---- walk the row above (0x56ccf0) ------------------------------------
    while (aboveStart <= belowStop)
    {
        const int aboveZone = ZoneOf(aboveStart);

        // The cell of the row above that this cell is compared against: the one
        // straight up, or - near the row's right end - the one up-and-left.
        uint8_t* abovePeer;
        if (aboveStart >= belowStop - 4)
            abovePeer = (aboveStart == belowStop - 4) ? aboveStart + rowSpan
                                                      : aboveStart + rowSpan - 4;
        else
            abovePeer = aboveStart + upStep;

        if (aboveZone != 0)
        {
            if (aboveZone != zone && aboveZone != lastRecordedZone_)
            {
                const int diff = aboveStart[1] - abovePeer[1];
                if ((diff < 2 && diff > -2) || wildcardLevel)
                {
                    RecordZonePair(zone, aboveZone);
                    lastRecordedZone_ = aboveZone;
                }
            }
        }
        else if (aboveStart[0] == startZone)
        {
            const int diff = aboveStart[1] - abovePeer[1];
            if (diff < 2 && diff > -2)
            {
                runStart += FillZoneFromRow(aboveStart, zone, &aboveOut);
                aboveStart += 4 * aboveOut;
                if (aboveStart > belowStop)
                    break;
                continue;
            }
        }
        aboveStart += 4;
    }

    // ---- walk the row below (0x56cf00) ------------------------------------
    // The walk ends at aboveEnd (vanilla's v84), not at belowEnd - the two rows
    // are offset by upStep, so this covers the row below the run and one cell
    // past its right end.
    uint8_t* belowWalk = belowStart;
    while (belowWalk <= aboveEnd)
    {
        const int belowZone = ZoneOf(belowWalk);

        // Mirror of the above-row peer: the cell straight down, or - near the
        // row's left end - the one down-and-right.
        uint8_t* belowPeer;
        if (belowWalk >= belowEnd - 4)
        {
            const int step = (belowWalk != belowEnd - 4) ? upStep : rowSpan;
            belowPeer = belowWalk - step;
        }
        else
        {
            belowPeer = belowWalk - rowSpan + 4;
        }

        if (belowZone != 0)
        {
            if (belowZone != zone && belowZone != lastRecordedZone_)
            {
                const int diff = belowWalk[1] - belowPeer[1];
                if ((diff < 2 && diff > -2) || wildcardLevel)
                {
                    RecordZonePair(zone, belowZone);
                    lastRecordedZone_ = belowZone;
                }
            }
            belowWalk += 4;
        }
        else if (belowWalk[0] == startZone
                 && belowWalk[1] - belowPeer[1] < 2
                 && belowWalk[1] - belowPeer[1] > -2)
        {
            runStart += FillZoneFromRow(belowWalk, zone, &belowOut);
            belowWalk += 4 * belowOut;
        }
        else
        {
            belowWalk += 4;
        }
    }

    return runStart;
}

// ---------------------------------------------------------------------------
// RollGlobalOptions - the rolls sub_596300 makes when the random-map dialog's
// "start generation" button is pressed (WM_COMMAND id 0x621, 0x59678f - 0x59683c).
// See GlobalMapOptions in MapGen.h for the order and the meaning of each value.
//
// The engine's global Randomizer (this_pRandomizer, 0x886B88) was seeded once
// at Game::Start from GetTickCount() (0x52fdf4), so a fresh run rolls fresh
// values - which is why the retail game produces a slightly different map each
// time. Passing the seed the game logged ("Seed is %08x") reproduces a run.
// ---------------------------------------------------------------------------
GlobalMapOptions RandomMapGenerator::RollGlobalOptions(uint32_t seed)
{
    globalRng_.Seed(seed);

    GlobalMapOptions opt;
    opt.flag010  = (globalRng_.RandomRanged(0, 100) < 50) ? 1 : 0;
    opt.n3       = globalRng_.RandomRanged(1, 4);      // 1..3
    opt.value020 = globalRng_.RandomRanged(0, 3);
    opt.value018 = globalRng_.RandomRanged(0, 3);
    opt.value03C = globalRng_.RandomRanged(0, 3);

    // ---- sub_597260(n3): the RMG option rolls (0x597282 - 0x597372) --------
    // Eight RandomRanged draws, in this exact order. this[21] = 20 * this[16]
    // sits between the fifth and the sixth and consumes none.
    //
    // The tables are the game's; only their [n3] entry is used. See
    // GlobalMapOptions for why the vanilla's values here are not reproducible
    // (the draws come from the session-wide randomizer).
    //
    // The lower bounds of this[24] and this[27] are not tables in .rdata but two
    // .data arrays, dword_ABED40 and dword_ABED18 (0x5972a9 / 0x5972c6). Both are
    // all zero in the image and the only instructions that touch them are those
    // two reads inside sub_597260, so the bound IS 0 - verified, not assumed.
    static const int kWaterLo[5] = {  75,   0,  50,   0,   0 };
    static const int kWaterHi[5] = { 100,  25, 100, 100, 100 };
    static const int k024Hi[5]   = { 100, 100, 100,   0,  20 };
    // Accessibility (this[27], the Making-regions ramp gate read at 0x5907b8):
    // vanilla rolls it in [0, k027Hi]. A different-Level region pair earns
    // 2-3 ramp slots only when its own roll < this gate, otherwise it is held
    // to the single guaranteed ramp. Inland mean is 50, but Mountainous is
    // only 10 ([0,20]), so on Mountainous maps almost every cliff run between
    // two associated ramps stays a vertical wall instead of stepping down with
    // another ramp. Tuned 2026-10-01 (user request): raise the lower bound for
    // the two cliff-bearing land types and widen the Mountainous range, so
    // each map reliably carries several cliff ramps. Non-cliff types 0/1/2
    // stay byte-identical to vanilla.
    static const int k027Lo[5]   = {   0,   0,   0,  35,  30 };
    static const int k027Hi[5]   = { 100, 100, 100, 100,  55 };
    static const int k028Lo[5]   = {  50,   0,  35,   0,   0 };
    static const int k028Hi[5]   = { 100, 100, 100, 100,  50 };

    const int index = (opt.n3 >= 0 && opt.n3 < 5) ? opt.n3 : 4;

    opt.waterAmount     = globalRng_.RandomRanged(kWaterLo[index],
                                                  kWaterHi[index]); // 0x597282
    opt.ruggedness      = globalRng_.RandomRanged(20, 100);         // 0x59729f
    opt.urbanPresence   = globalRng_.RandomRanged(0, k024Hi[index]); // 0x5972bc
    opt.accessibility   = globalRng_.RandomRanged(k027Lo[index],
                                                  k027Hi[index]); // 0x5972d9
    opt.regionSize      = globalRng_.RandomRanged(k028Lo[index],
                                                  k028Hi[index]);   // 0x5972f6
    // 0x59730b: this[21] = 20 * this[16] - no draw.

    opt.tiberiumLayout = globalRng_.RandomRanged(0, 100);           // 0x597313

    // this[23]: RMGVegetationMinimums / Maximums[n3], each clamped to 0..100
    // and with lo = min(lo, hi) (0x597322 - 0x597350). With the shipped
    // rmgmd.ini both lists are all 60 / all 100, so this is 60..100 for every
    // map type.
    int vegLo = settings_.VegetationMin(opt.n3);
    int vegHi = settings_.VegetationMax(opt.n3);
    if (vegLo < 0) vegLo = 0; else if (vegLo > 100) vegLo = 100;
    if (vegHi < 0) vegHi = 0; else if (vegHi > 100) vegHi = 100;
    if (vegLo > vegHi) vegLo = vegHi;
    opt.vegetation = globalRng_.RandomRanged(vegLo, vegHi);        // 0x59736a

    opt.seed04C = globalRng_.RandomRanged(0, 0xFFFF);              // 0x59736d

    globalOptions_ = opt;
    return opt;
}

// ---------------------------------------------------------------------------
// The stage body (0x598e9e - 0x598ebe).
// ---------------------------------------------------------------------------
void RandomMapGenerator::CreateStartingPoints()
{
    while (true)
    {
        if (!SelectStartingPointRegions())     // 0x598eab  sub_594B50
            continue;
        if (!PaintStartingPointTerrain())      // 0x598eb6  sub_5A1FB0
            continue;
        break;
    }
}

// ---------------------------------------------------------------------------
// Step 1 - sub_594B50 (0x594B50 - 0x5953FF)
//
// Picks the regions the starting points sit in:
//
//   1. prepares the five MovementZones (3 x Amphibious, 2 x Normal) and selects
//      one through the global n3 (0xABE014) - the same global the tech-building
//      stage gates on;
//   2. throws away every region smaller than
//      max(dword_ABE15C * dword_ABE158 * 0.03, 400.0);
//   3. seeds regions over the cells that pass sub_5AC230, carry no region mark
//      (work +14 == -1), are Passable and whose move error matches the reference
//      read through sub_56C510;
//   4. ranks the survivors by id + regionCount * (500000 - cellCount);
//   5. splits the global starting-point budget (dword_ABE028) across the
//      selected regions in proportion to their CUMULATIVE cellCount: each
//      share is F2I64(running / total * budget + 0.5) minus what is already
//      spent (subtracting the integer spent commutes with the truncation), and
//      the last region takes the remainder so no point is lost to rounding;
//   6. stamps each region's cells (sub_594870) and, when n3 == 0, coin-flips an
//      extra point into every region that ended up with a zero share.
//
// Always answers true.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::SelectStartingPointRegions()
{
    // ---- 1. movement zone (0x594b53 - 0x594b9b) ---------------------------
    // The vanilla builds a five-slot table and indexes it with the global n3:
    //     nMovementZone[5] = { 5, 5, 5, 0, 0 }     (0x594b59: eax = 5 for the
    //                                               first three, 0 for the rest)
    //     selected = nMovementZone[n3]             (n3 is 1..3)
    // so n3 == 3 lands on 0 and n3 == 1 or 2 land on 5. The value indexes
    // MapClass::MovementZones.
    static const int kZoneTable[5] = { 5, 5, 5, 0, 0 };
    const int n3 = config_.global.n3;
    const int movementZone = kZoneTable[(n3 >= 0 && n3 < 5) ? n3 : 0];

    // ---- 2. the reference move error (0x594b62 - 0x594bb0) ----------------
    // sub_56C510 rebuilds the zones and answers the number of the LARGEST zone;
    // that number indexes the selected zone's cost table to give the value every
    // seeded cell must match.
    const int largestZone = RebuildMoveZones();
    int referenceError = 0;
    if (movementZone >= 0 && movementZone < 13)
    {
        const std::vector<int>& table = movementZones_[movementZone];
        if (largestZone >= 0 && largestZone < static_cast<int>(table.size()))
            referenceError = table[static_cast<size_t>(largestZone)];
    }

    // ---- 3. the region size floor (0x594b53 - 0x594b9b) -------------------
    //     number = this[97] (map HEIGHT) * this[96] (map WIDTH) * 0.03
    //     if (number < 400) number = 400;   minCells = F2I64(number)
    // Both are size_ fields, not the constant 0 an earlier reading used (see the
    // file header): 70*70*0.03 = 147 -> floor 400, 120*120*0.03 = 432.
    double number = static_cast<double>(size_.height)
                  * static_cast<double>(size_.width) * 0.03;
    if (number < 400.0)
        number = 400.0;
    const int minCells = static_cast<int>(number);         // F2I64 truncates

    // ---- 4. clear every region (0x594bc0, sub_58CE90) ---------------------
    // The vanilla first zeroes both work marks of every cell, then tears the
    // region objects down and resets the id counter.
    {
        const int side = size_.workSide;
        const int workCount = side * side;
        for (int idx = 0; idx < workCount; ++idx)
        {
            workCells_[idx].data[14] = -1;
            workCells_[idx].data[15] = -1;
        }
        for (int i = static_cast<int>(regions_.size()) - 1; i >= 0; --i)
        {
            if (regions_[i] != nullptr)
                ReleaseRegionObject(regions_[i], true);
        }
        regionIdCounter_ = 0;
    }

    // ---- 5. seed one region per qualifying cell (0x594be0 - 0x594e00) -----
    // Every diamond cell that is in the usable diamond, carries no region mark,
    // is Passable and whose move error equals the reference value starts a
    // region (sub_594420 flood-fills it).
    {
        int diagInDiamond = 0;
        int diagPassable = 0;
        int diagMatchErr = 0;
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth);
        while (MapCell* cell = it.Next())
        {
            const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
            const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);

            if (!CellExists(x, y))
                continue;
            ++diagInDiamond;
            if (cell->Passability != PassabilityType_Passable)
                continue;
            ++diagPassable;
            if (GetMoveError(CellStruct{ x, y }, movementZone) != referenceError)
                continue;
            ++diagMatchErr;

            if (workCells_[x + size_.workSide * y].data[14] != -1)
                continue;

            SeedRegionFromCell(cell, movementZone, referenceError);
        }
        DiagLog("SP-CELLS inDiamond=%d passable=%d passableAndErrMatch=%d refErr=%d",
                diagInDiamond, diagPassable, diagMatchErr, referenceError);
    }

    // 切断主因诊断：全菱形格按 Passability 类别计数；纯可通行格(Passable=0)
    // 里 GetMoveError 的取值分布（=referenceError 的才是出生点可用平地，
    // 其余就是把平地切开的带：水/沙滩/树等在 Normal 区被归到别的代价类）。
    {
        int passHist[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int errMatch = 0;
        int errOther = 0;
        int errOtherVals[16] = { 0 };
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth);
        while (MapCell* cell = it.Next())
        {
            const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
            const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);
            if (!CellExists(x, y))
                continue;
            int pc = cell->Passability;
            if (pc < 0 || pc > 7)
                pc = 7;
            ++passHist[pc];
            if (cell->Passability == PassabilityType_Passable)
            {
                const int e = GetMoveError(CellStruct{ x, y }, movementZone);
                if (e == referenceError)
                    ++errMatch;
                else
                {
                    ++errOther;
                    if (e >= 0 && e < 16)
                        ++errOtherVals[e];
                }
            }
        }
        DiagLog("SP-PASS pass0=%d crush1=%d destroy2=%d beach3=%d water4=%d free5=%d impass6=%d outside7=%d",
                passHist[0], passHist[1], passHist[2], passHist[3],
                passHist[4], passHist[5], passHist[6], passHist[7]);
        std::string errDist = "SP-ERRDIST";
        for (int e = 0; e < 16; ++e)
            if (errOtherVals[e] > 0)
                errDist += " e" + std::to_string(e) + "=" + std::to_string(errOtherVals[e]);
        DiagLog("%s (passableErrMatch=%d passableErrOther=%d)",
                errDist.c_str(), errMatch, errOther);
    }

    // ---- 6. cull the small regions (0x594e10 - 0x594e78) ------------------
    int seededRegions = 0;
    int maxSeenCells = 0;
    int hist200 = 0, hist300 = 0, hist400 = 0;
    for (size_t i = 0; i < regions_.size(); ++i)
    {
        if (regions_[i] == nullptr)
            continue;
        ++seededRegions;
        const int cc = regions_[i]->cellCount;
        if (cc > maxSeenCells)
            maxSeenCells = cc;
        if (cc >= 200) ++hist200;
        if (cc >= 300) ++hist300;
        if (cc >= 400) ++hist400;
    }
    for (int i = static_cast<int>(regions_.size()) - 1; i >= 0; --i)
    {
        MapRegion* region = regions_[i];
        if (region != nullptr && region->cellCount < minCells)
            ReleaseRegionObject(region, true);
    }
    DiagLog("SP-SIZE maxRegionCells=%d  >=200:%d >=300:%d >=400:%d",
            maxSeenCells, hist200, hist300, hist400);

    // 隔离带诊断：对"Passable 且 err=referenceError"的格按 8 邻域一次性连通分块
    // （与 SeedRegionFromCell 同口径），给每格记块号；再对最大 5 块统计其边界外侧
    // 相邻的非合格格类别，看清平地主要被什么切开。
    {
        const int side = size_.workSide;
        static const int16_t kDX[8] = { 0,1,1,1,0,-1,-1,-1 };
        static const int16_t kDY[8] = { -1,-1,0,1,1,1,0,-1 };

        auto isGood = [&](int x, int y) -> bool
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                return false;
            const MapCell* c = CellAt(x, y);
            return c->Passability == PassabilityType_Passable
                && GetMoveError(CellStruct{ static_cast<int16_t>(x),
                                            static_cast<int16_t>(y) },
                                movementZone) == referenceError;
        };

        std::vector<int> blockOf(static_cast<size_t>(side) * side, -1);
        std::vector<int> blockSize;
        for (int sy = 0; sy < side; ++sy)
        {
            for (int sx = 0; sx < side; ++sx)
            {
                const int seedIdx = sx + side * sy;
                if (blockOf[seedIdx] != -1 || !isGood(sx, sy))
                    continue;
                const int bid = static_cast<int>(blockSize.size());
                blockSize.push_back(0);
                std::vector<int> st;
                st.push_back(seedIdx);
                blockOf[seedIdx] = bid;
                while (!st.empty())
                {
                    const int idx = st.back();
                    st.pop_back();
                    ++blockSize[bid];
                    const int px = idx % side;
                    const int py = idx / side;
                    for (int d = 0; d < 8; ++d)
                    {
                        const int nx = px + kDX[d];
                        const int ny = py + kDY[d];
                        if (nx < 0 || ny < 0 || nx >= side || ny >= side)
                            continue;
                        const int ni = nx + side * ny;
                        if (blockOf[ni] != -1 || !isGood(nx, ny))
                            continue;
                        blockOf[ni] = bid;
                        st.push_back(ni);
                    }
                }
            }
        }

        std::vector<std::pair<int,int> > bySize;
        for (size_t bi = 0; bi < blockSize.size(); ++bi)
            bySize.push_back(std::make_pair(static_cast<int>(bi), blockSize[bi]));
        std::sort(bySize.begin(), bySize.end(),
                  [](const std::pair<int,int>& a, const std::pair<int,int>& b)
                  { return a.second > b.second; });

        for (int rank = 0; rank < 5 && rank < static_cast<int>(bySize.size()); ++rank)
        {
            const int bid = bySize[rank].first;
            int nbBeach=0, nbWater=0, nbImpass=0, nbE1=0, nbOut=0, nbOther=0;
            for (int idx = 0; idx < side*side; ++idx)
            {
                if (blockOf[idx] != bid)
                    continue;
                const int px = idx % side;
                const int py = idx / side;
                for (int d = 0; d < 8; ++d)
                {
                    const int nx = px + kDX[d], ny = py + kDY[d];
                    if (nx<0||ny<0||nx>=side||ny>=side) { ++nbOut; continue; }
                    if (!CellExists(static_cast<int16_t>(nx), static_cast<int16_t>(ny)))
                    { ++nbOut; continue; }
                    const MapCell* nc = CellAt(nx, ny);
                    if (isGood(nx, ny)) continue;          // 合格平地（块内/别的块）
                    if (nc->Passability == PassabilityType_Beach) ++nbBeach;
                    else if (nc->Passability == PassabilityType_Water) ++nbWater;
                    else if (nc->Passability == PassabilityType_Impassable) ++nbImpass;
                    else if (nc->Passability == PassabilityType_OutsideMap) ++nbOut;
                    else if (nc->Passability == PassabilityType_Passable) ++nbE1;
                    else ++nbOther;
                }
            }
            DiagLog("SP-BLOCK rank=%d size=%d neighbours: beach=%d water=%d impass=%d otherFlat=%d outside=%d other=%d",
                    rank + 1, bySize[rank].second,
                    nbBeach, nbWater, nbImpass, nbE1, nbOut, nbOther);
        }
    }
    int survivingRegions = 0;
    for (size_t i = 0; i < regions_.size(); ++i)
        if (regions_[i] != nullptr)
            ++survivingRegions;
    DiagLog("SP-REGIONS n3=%d zone=%d refErr=%d minCells=%d seeded=%d surviving=%d",
            n3, movementZone, referenceError, minCells,
            seededRegions, survivingRegions);

    // ---- 7. rank, select and split the starting-point budget --------------
    // (0x594e80 - 0x5952xx). Ranked by `id + regionCount * (500000 - cellCount)`,
    // i.e. the biggest regions first; the global budget dword_ABE028 is then
    // handed out in proportion to cellCount, the LAST region taking whatever is
    // left so rounding cannot lose a point.
    std::vector<MapRegion*> selected;
    {
        // vanilla keeps at most 10 candidates (sub_5AD930(10))
        std::vector<std::pair<int, MapRegion*> > ranked;
        ranked.reserve(regions_.size());
        const int regionCount = static_cast<int>(regions_.size());
        for (size_t i = 0; i < regions_.size(); ++i)
        {
            MapRegion* r = regions_[i];
            if (r == nullptr)
                continue;
            const int score = r->id + regionCount * (500000 - r->cellCount);
            ranked.push_back(std::make_pair(score, r));
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const std::pair<int, MapRegion*>& a,
                            const std::pair<int, MapRegion*>& b)
                         {
                             return a.first < b.first;
                         });

        const size_t take = ranked.size() < 10 ? ranked.size() : 10;
        for (size_t i = 0; i < take; ++i)
            selected.push_back(ranked[i].second);
    }

    {
        long long total = 0;
        for (size_t i = 0; i < selected.size(); ++i)
            total += selected[i]->cellCount;

        const long long budget = config_.playerCount;    // dword_ABE028 = NumPlayers
        long long running = 0;
        long long spent = 0;
        for (size_t i = 0; i < selected.size(); ++i)
        {
            running += selected[i]->cellCount;           // 0x5951xx: v21 += cellCount

            long long share;
            if (i + 1 == selected.size())
            {
                share = budget - spent;                  // the last takes the rest
            }
            else
            {
                // F2I64(running / total * budget + 0.5 - spent). The vanilla
                // carries `spent` in the high dword of the same 64-bit value
                // that holds the budget, which is where the subtraction comes
                // from; subtracting it outside the truncation is the same thing
                // because it is an integer.
                share = (total > 0)
                          ? F2I64(static_cast<double>(running)
                                  / static_cast<double>(total)
                                  * static_cast<double>(budget) + 0.5) - spent
                          : 0;
            }
            selected[i]->startingPoints = static_cast<int>(share);
            spent += share;
            DiagLog("SP-SELECT rank=%d total=%lld budget=%lld region id=%d cells=%d share=%lld",
                    static_cast<int>(i), total, budget,
                    selected[i]->id, selected[i]->cellCount, share);
        }
    }
    if (selected.empty())
        DiagLog("SP-SELECT no region passed the size floor - budget cannot be assigned");

    // ---- 8. stamp the starting-point cells (0x5952xx, sub_594870) ---------
    // The offset is the running starting-point number, and it advances by the
    // region's share even when the region could not supply that many picks.
    int offset = 0;
    for (size_t i = 0; i < selected.size(); ++i)
    {
        StampStartingPointCells(selected[i], offset);
        offset += selected[i]->startingPoints;
    }
    startingPointCount_ = offset;

    // ---- 8b. 稳定出生点兜底：严格口径一个点都没放出来时，放宽重圈 ----------
    // 原版严格要求"纯可通行 + 移动代价==最大分量"。在我们这版山地（坡道密度被
    // 定制提高）上，崖/坡会把同一片可行走平地切到不同移动分量里，最大分量被
    // 切成一堆 <400 格的碎块，名额无处可分（实测种子 0x524E55BC：最大块仅 327
    // 格，另有 1090 格可行走平地落在别的分量）。此时放宽为"只认纯可通行平地"，
    // 让相邻的不同分量平地合并成大块，再按原流程分名额、落点。这只改变出生点
    // 选址阶段的圈地口径，不改地形，也不影响正常图（正常图严格口径已放点，不
    // 会进到这里）。
    if (startingPoints_.empty())
    {
        startpointRelaxedMove_ = true;
        DiagLog("SP-RELAX strict pass produced 0 start points - reseeding on plain-passable ground");

        // 清场（同第 4 步）
        {
            const int side2 = size_.workSide;
            const int workCount2 = side2 * side2;
            for (int idx = 0; idx < workCount2; ++idx)
            {
                workCells_[idx].data[14] = -1;
                workCells_[idx].data[15] = -1;
            }
            for (int i = static_cast<int>(regions_.size()) - 1; i >= 0; --i)
                if (regions_[i] != nullptr)
                    ReleaseRegionObject(regions_[i], true);
            regionIdCounter_ = 0;
        }

        // 放宽播种：只要求在菱形内、无标记、纯可通行（flood 同样忽略移动分量）
        {
            CellIterator it;
            it.Reset(cellSlots_, size_.mapWidth);
            while (MapCell* cell = it.Next())
            {
                const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
                const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);
                if (!CellExists(x, y))
                    continue;
                if (workCells_[x + size_.workSide * y].data[14] != -1)
                    continue;
                if (cell->Passability != PassabilityType_Passable)
                    continue;
                SeedRegionFromCell(cell, movementZone, referenceError);
            }
        }

        // 剔除小块。同高平面可能比原版 400 格门槛还碎；门槛取 min(400, 最大块
        // 大小)，保证至少最大的那块同高平面入选（否则又会 0 出生点）。
        int maxRelaxCells = 0;
        for (size_t i = 0; i < regions_.size(); ++i)
            if (regions_[i] != nullptr && regions_[i]->cellCount > maxRelaxCells)
                maxRelaxCells = regions_[i]->cellCount;
        const int relaxFloor = (maxRelaxCells < minCells) ? maxRelaxCells : minCells;
        for (int i = static_cast<int>(regions_.size()) - 1; i >= 0; --i)
        {
            MapRegion* region = regions_[i];
            if (region != nullptr && region->cellCount < relaxFloor)
                ReleaseRegionObject(region, true);
        }
        DiagLog("SP-RELAX-FLOOR floor=%d maxSameLevelCells=%d", relaxFloor, maxRelaxCells);

        // 排序取前 10
        std::vector<MapRegion*> relaxedSelected;
        {
            std::vector<std::pair<int, MapRegion*> > ranked;
            ranked.reserve(regions_.size());
            const int regionCount = static_cast<int>(regions_.size());
            for (size_t i = 0; i < regions_.size(); ++i)
            {
                MapRegion* r = regions_[i];
                if (r == nullptr)
                    continue;
                ranked.push_back(std::make_pair(
                    r->id + regionCount * (500000 - r->cellCount), r));
            }
            std::stable_sort(ranked.begin(), ranked.end(),
                [](const std::pair<int, MapRegion*>& a,
                   const std::pair<int, MapRegion*>& b)
                { return a.first < b.first; });
            const size_t take = ranked.size() < 10 ? ranked.size() : 10;
            for (size_t i = 0; i < take; ++i)
                relaxedSelected.push_back(ranked[i].second);
        }

        // 分名额：放宽口径下各块之间隔着水/崖，彼此没有陆路（8 邻域都不相连才
        // 会被分成两块）。若按格数把玩家拆到不同块，出生点之间就无法陆地通行。
        // 因此全部名额都给最大的那一块（主大陆），其余块名额为 0 —— 保证所有
        // 玩家落在同一片正交连通的可行走地面上。块内选点仍做最远分散。
        {
            const long long budget = config_.playerCount;
            for (size_t i = 0; i < relaxedSelected.size(); ++i)
            {
                const long long share = (i == 0) ? budget : 0;
                relaxedSelected[i]->startingPoints = static_cast<int>(share);
                DiagLog("SP-RELAX-SELECT rank=%d budget=%lld region id=%d cells=%d share=%lld%s",
                        static_cast<int>(i), budget,
                        relaxedSelected[i]->id, relaxedSelected[i]->cellCount, share,
                        i == 0 ? " (mainland, all players)" : " (separated by water/cliff, 0)");
            }
        }

        // 重放出生点（先清掉严格流程可能留下的空账与路标计数）
        startingPoints_.clear();
        offset = 0;
        for (size_t i = 0; i < relaxedSelected.size(); ++i)
        {
            StampStartingPointCells(relaxedSelected[i], offset);
            offset += relaxedSelected[i]->startingPoints;
        }
        startingPointCount_ = offset;
        DiagLog("SP-RELAX done: regions=%d startPoints=%d",
                static_cast<int>(relaxedSelected.size()),
                static_cast<int>(startingPoints_.size()));

        startpointRelaxedMove_ = false;
    }

    // ---- 9. the zero-share fallback (0x5953xx) ----------------------------
    // When n3 == 0 every region that ended up with no point gets one more chance
    // on a coin flip. n3 is 1..3 here (never 0), so this is unreachable from the
    // dialog path - the branch is kept so the shape matches.
    if (n3 == 0)
    {
        for (size_t i = 0; i < selected.size(); ++i)
        {
            if (selected[i]->startingPoints == 0
                && rng_.Unit() < 0.5)
            {
                GiveRegionATechBuilding(selected[i]);   // sub_595400
            }
        }
    }

    // ---- 10. 出生点连通性保证（端口定制，不动 CreateHills）----------------
    // 上面的名额分配可能把玩家点放到不同区域；即便它们移动代价同类，也可能在
    // 8 邻同高意义上并不相邻（隔着别的格类）。放完点立刻在当前高度场校验：
    // 所有出生点必须落在同一个"可通行 + 同高程 + 正交连通"平面（平面内部零
    // 高差，单位必然直达，不依赖斜坡朝向）。不满足就把全部点收进最大的那块
    // 同高平面重新分散放置。
    EnsureStartingPointsConnected();

    return true;
}

bool RandomMapGenerator::IsCliffClearAround(CellStruct anchor, int radius)
{
    if (!CellExists(anchor.X, anchor.Y))
        return false;
    const int baseLevel = CellAt(anchor.X, anchor.Y)->Level;

    for (int cy = anchor.Y - radius; cy <= anchor.Y + radius; ++cy)
    {
        for (int cx = anchor.X - radius; cx <= anchor.X + radius; ++cx)
        {
            const int16_t ax = static_cast<int16_t>(cx);
            const int16_t ay = static_cast<int16_t>(cy);
            if (!CellExists(ax, ay))
                continue;
            const MapCell* q = CellAt(ax, ay);
            const int diff = q->Level - baseLevel;
            if (q->SlopeIndex != 0
                || q->Passability == PassabilityType_Impassable
                || diff >= 2 || diff <= -2)
            {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// BuildCoastalDistance [port-only]
//
// 大岛屿出生点"靠海"判定用的步数表。原版 RMG 没有这条规则，是按用户口径定制：
// 出生点必须能在陆路上 15 步（kCoastalSteps）以内走到海边。
//
//   1. 先圈"外海"：从"贴着地图外圈"的水格出发——外圈指不可用区
//      （Passability == OutsideMap，即 IsWithinUsableArea 之外的那一圈）。
//      只在 Passability == Water 的格之间做正交泛洪。连不到外圈的水（岛内
//      湖泊、河流）自然落选，正好对应"不算岛屿内部的水体"。
//      注意：不能拿"邻格越出菱形(CellExists=false)"当边界——实测那张大岛屿图
//      最外一圈全是 OutsideMap 不是水，水格一个都贴不到菱形边，会圈出 0 个
//      海格（rmg_20261003_210359 的 seaCells=0、候选全被否掉就是这么来的）。
//   2. 再以贴着外海的陆军格为种子，用与出生点连通完全相同的陆军口径向外做多
//      源 BFS：可踩 = Passable/Beach；同 Level 直通；相差 1 必须有一格是真坡
//      （SlopeIndex 非 0）；差 2 及以上不通。记录每个可走格到海边的步数。
//
// 外海格记 0，贴海陆格记 1，其余按步数递增；走不到的格保持 -1。
// ---------------------------------------------------------------------------
static const int kCoastalSteps = 15;

std::vector<int> RandomMapGenerator::BuildCoastalDistance()
{
    const int side = size_.workSide;
    const size_t total = static_cast<size_t>(side) * static_cast<size_t>(side);
    static const int16_t kDX[4] = { 0, 1, 0, -1 };
    static const int16_t kDY[4] = { -1, 0, 1, 0 };

    std::vector<uint8_t> sea(total, 0);
    std::vector<int> distance(total, -1);
    std::vector<int> queue;

    // ---- 1. 外海泛洪（只走水格，且必须贴着地图外圈 OutsideMap）-------------
    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth);
    while (MapCell* cell = it.Next())
    {
        if (cell->Passability != PassabilityType_Water)
            continue;
        const int x = cell->MapCoords & 0xFFFF;
        const int y = static_cast<uint32_t>(cell->MapCoords) >> 16;
        bool onRim = false;
        for (int d = 0; d < 4 && !onRim; ++d)
        {
            const int16_t nx = static_cast<int16_t>(x + kDX[d]);
            const int16_t ny = static_cast<int16_t>(y + kDY[d]);
            if (!CellExists(nx, ny))
                continue;
            if (CellAt(nx, ny)->Passability == PassabilityType_OutsideMap)
                onRim = true;
        }
        if (!onRim)
            continue;
        const int idx = x + side * y;
        if (sea[idx])
            continue;
        sea[idx] = 1;
        queue.push_back(idx);
    }

    for (size_t head = 0; head < queue.size(); ++head)
    {
        const int idx = queue[head];
        const int x = idx % side;
        const int y = idx / side;
        for (int d = 0; d < 4; ++d)
        {
            const int16_t nx = static_cast<int16_t>(x + kDX[d]);
            const int16_t ny = static_cast<int16_t>(y + kDY[d]);
            if (!CellExists(nx, ny))
                continue;
            const int nidx = nx + side * ny;
            if (sea[nidx] || CellAt(nx, ny)->Passability != PassabilityType_Water)
                continue;
            sea[nidx] = 1;
            queue.push_back(nidx);
        }
    }
    const int seaCells = static_cast<int>(queue.size());

    // 找不到外海（极端情况：整图无水）时返回空表，本轮不启用靠海约束。否则空表
    // 会让每个候选点都判成"走不到海"，把候选全否掉，连拉远距离都做不成。
    if (seaCells == 0)
    {
        DiagLog("SP-COAST seaCells=0 - coastal constraint skipped this round");
        return std::vector<int>();
    }

    // ---- 2. 陆军步数 BFS：种子 = 贴着外海的可走陆格 ------------------------
    queue.clear();
    const auto walkableLand = [](const MapCell* c) -> bool
    {
        return c != nullptr
            && (c->Passability == PassabilityType_Passable
                || c->Passability == PassabilityType_Beach);
    };

    CellIterator seedIt;
    seedIt.Reset(cellSlots_, size_.mapWidth);
    while (MapCell* cell = seedIt.Next())
    {
        const int x = cell->MapCoords & 0xFFFF;
        const int y = static_cast<uint32_t>(cell->MapCoords) >> 16;
        if (!walkableLand(cell))
            continue;
        for (int d = 0; d < 4; ++d)
        {
            const int16_t nx = static_cast<int16_t>(x + kDX[d]);
            const int16_t ny = static_cast<int16_t>(y + kDY[d]);
            if (!CellExists(nx, ny) || !sea[nx + side * ny])
                continue;
            const int idx = x + side * y;
            if (distance[idx] >= 0)
                break;
            distance[idx] = 1;
            queue.push_back(idx);
            break;
        }
    }

    for (size_t head = 0; head < queue.size(); ++head)
    {
        const int idx = queue[head];
        const int x = idx % side;
        const int y = idx / side;
        const MapCell* a = CellAt(x, y);
        for (int d = 0; d < 4; ++d)
        {
            const int16_t nx = static_cast<int16_t>(x + kDX[d]);
            const int16_t ny = static_cast<int16_t>(y + kDY[d]);
            if (!CellExists(nx, ny))
                continue;
            const MapCell* b = CellAt(nx, ny);
            if (!walkableLand(b))
                continue;
            const int diff = b->Level - a->Level;
            if (diff != 0
                && !((diff == 1 || diff == -1)
                     && (a->SlopeIndex != 0 || b->SlopeIndex != 0)))
                continue;
            const int nidx = nx + side * ny;
            if (distance[nidx] >= 0)
                continue;
            distance[nidx] = distance[idx] + 1;
            queue.push_back(nidx);
        }
    }

    DiagLog("SP-COAST seaCells=%d walkable=%zu",
            seaCells, queue.size());
    return distance;
}

// ---------------------------------------------------------------------------
// RelocateStartingPointsLandConnected [port-only]
//
// 旧版 EnsureStartingPointsConnected 把全部出生点强行收进同一个"同高程正交
// 连通平面"：一个点在崖下、另一个点在崖上，即使两者之间刻了斜坡也不算连通。
// 本函数改用真正的陆军陆路口径分块：
//
//   两正交相邻格 A、B 可步行连通，当且仅当
//     1. 两格都是陆军可踩：Passability 为 Passable 或 Beach（水/不可通过/越界
//        一律不行——实测崖墙瓦片(CliffSet 49..88)在 Recalc 后全部是
//        Impassable/OutsideMap，所以崖墙天然挡住泛洪，只有坡瓦漏得过来）；
//     2. 两格 Level 相等 —— 直接连通；
//     3. 两格 Level 相差恰好 1 —— 必须有一格 SlopeIndex != 0，即真斜坡。
//        坡道雕刻给每级坡带写的就是逐级 -1 的 Level + 非 0 SlopeIndex
//        （RampBuilder: ownerLevel-n-1），造丘陵阶段的 ±1 微坡同理；没有坡瓦
//        的一档直壁仍是断墙，不放行；
//     4. Level 相差 2 及以上 —— 不连通（两级及以上无单格坡可跨越）。
//
// 这与原版移动区泛洪 sub_56CB90 的邻格判据（|level diff| < 2 且同类可行走）
// 同口径，再补一条"跨级必须有坡瓦"以严格排除无坡直壁。只用正交接壤，斜对
// 角擦着不算连通（与出生点圈地的既有口径一致）。
//
// 选定陆块后，在陆块内部所有"可通行 + 自身平(SlopeIndex=0) + 6x6 同高净空"
// 的候选里，按旧版同样的几何最远对 + 贪心规则分散 players 个点，所以不同出
// 生点可以落在不同高度的台面上，彼此仍有斜坡陆路可达。
// ---------------------------------------------------------------------------
bool RandomMapGenerator::RelocateStartingPointsLandConnected(int players)
{
    const int side = size_.workSide;
    static const int16_t kDX[4] = { 0, 1, 0, -1 };
    static const int16_t kDY[4] = { -1, 0, 1, 0 };

    const auto walkableLand = [](const MapCell* c) -> bool
    {
        return c != nullptr
            && (c->Passability == PassabilityType_Passable
                || c->Passability == PassabilityType_Beach);
    };

    // 判定正交邻格 (px,py)->(nx,ny) 是否为同一条陆军陆路（见函数头四条规则）。
    const auto landEdge = [&](const MapCell* a, int nx, int ny) -> bool
    {
        if (nx < 0 || ny < 0 || nx >= side || ny >= side)
            return false;
        if (!CellExists(static_cast<int16_t>(nx), static_cast<int16_t>(ny)))
            return false;
        const MapCell* b = CellAt(nx, ny);
        if (!walkableLand(b))
            return false;
        const int d = b->Level - a->Level;
        if (d == 0)
            return true;
        if (d == 1 || d == -1)
            return a->SlopeIndex != 0 || b->SlopeIndex != 0;
        return false;
    };

    // ---- 陆路连通分块（4 向泛洪）-----------------------------------------
    std::vector<int> compOf(static_cast<size_t>(side) * side, -1);
    std::vector<std::vector<CellStruct> > compCells;
    for (int sy = 0; sy < side; ++sy)
    {
        for (int sx = 0; sx < side; ++sx)
        {
            const int seedIdx = sx + side * sy;
            if (compOf[seedIdx] != -1)
                continue;
            if (!CellExists(static_cast<int16_t>(sx), static_cast<int16_t>(sy)))
                continue;
            const MapCell* seed = CellAt(sx, sy);
            if (!walkableLand(seed))
                continue;

            const int cid = static_cast<int>(compCells.size());
            compCells.push_back(std::vector<CellStruct>());
            std::vector<int> st;
            st.push_back(seedIdx);
            compOf[seedIdx] = cid;
            while (!st.empty())
            {
                const int idx = st.back();
                st.pop_back();
                const int px = idx % side;
                const int py = idx / side;
                compCells[cid].push_back(
                    CellStruct{ static_cast<int16_t>(px), static_cast<int16_t>(py) });
                const MapCell* cur = CellAt(px, py);
                for (int dir = 0; dir < 4; ++dir)
                {
                    const int nx = px + kDX[dir];
                    const int ny = py + kDY[dir];
                    if (nx < 0 || ny < 0 || nx >= side || ny >= side)
                        continue;
                    const int ni = nx + side * ny;
                    if (compOf[ni] != -1)
                        continue;
                    if (landEdge(cur, nx, ny))
                    {
                        compOf[ni] = cid;
                        st.push_back(ni);
                    }
                }
            }
        }
    }

    if (compCells.empty())
        return false;

    // ---- 选陆块：原点全在同一块就锁定它，否则取最大块 ----------------------
    int target = -1;
    int originComp = -1;
    bool allSame = true;
    for (size_t i = 0; i < startingPoints_.size(); ++i)
    {
        const CellStruct c = startingPoints_[i].coords;
        const int cid = CellExists(c.X, c.Y) ? compOf[c.X + side * c.Y] : -1;
        if (cid < 0) { allSame = false; break; }
        if (originComp < 0) originComp = cid;
        else if (cid != originComp) allSame = false;
    }
    if (allSame && originComp >= 0)
        target = originComp;
    else
        for (size_t i = 0; i < compCells.size(); ++i)
            if (target < 0 || compCells[i].size() > compCells[target].size())
                target = static_cast<int>(i);

    const std::vector<CellStruct>& land = compCells[target];

    // 陆块的高程跨度（日志用：这个陆块串起了几级台面）
    int loLevel = 1000, hiLevel = -1000;
    for (size_t i = 0; i < land.size(); ++i)
    {
        const int lv = CellAt(land[i].X, land[i].Y)->Level;
        if (lv < loLevel) loLevel = lv;
        if (lv > hiLevel) hiLevel = lv;
    }
    DiagLog("SP-LAND components=%zu target=%d cells=%d levelSpan=%d..%d originsSame=%s",
            compCells.size(), target, static_cast<int>(land.size()),
            loLevel, hiLevel, allSame ? "YES" : "NO");

    // ---- 枚举陆块内全部可放点：可通行、自身平、6x6 同高净空、在可见区内 ----
    // 另加"离悬崖缓冲"：6x6 净空只保证基地车有一块同高平地，点仍可能紧挨崖
    // 边（实测 rmg_20261003_185613 pt3：锚点自身平整，但北侧 3 格就是 L3 斜
    // 坡、东侧贴着 L8 崖顶台地）。再要求以锚点为中心 kCliffClear 格内没有斜
    // 坡(SlopeIndex!=0)、不可通行格(崖墙 Impassable) 或高差 >=2 的台地，让出
    // 生点和悬崖之间留出一圈平地。
    const int kCliffClear = 4;
    const int visX = kVisibleOffsetX + 4;
    const int visY = kVisibleOffsetY + 4;
    const int visW = size_.width - 8;
    const int visH = size_.height - 8;
    // 大岛屿硬约束：出生点必须能在陆路上 kCoastalSteps 步内走到海边（岛内湖泊
    // /河流不计入）。海图（群岛/大岛屿群）与内陆/山地不加这条。
    const bool needCoastal = (config_.landType == RmLandType::Continent);
    std::vector<int> coastalDist;
    if (needCoastal)
        coastalDist = BuildCoastalDistance();
    std::vector<CellStruct> candidates;
    candidates.reserve(land.size());
    for (size_t i = 0; i < land.size(); ++i)
    {
        const CellStruct picked = land[i];
        const MapCell* c = CellAt(picked.X, picked.Y);
        // 锚点必须是纯可通行（Beach 只能当路面不能摆基地车），且自身是平格。
        if (c->Passability != PassabilityType_Passable || c->SlopeIndex != 0)
            continue;
        // 6x6 同高净空：基地车展开需要一块与锚点同高程的平地，坡/崖只允许留在
        // 这块净空之外（连通由泛洪保证，玩家仍可经坡走到别的台面）。
        if (!TileRectLevelClear(picked.X - 3, picked.Y - 3, 6, 6, false))
            continue;
        if (!IsWithinUsableRect(picked, true, visX, visY, visW, visH))
            continue;

        // 悬崖缓冲圈：圈内不得出现坡瓦 / 不可通行崖墙 / 高差 >=2。
        if (!IsCliffClearAround(picked, kCliffClear))
            continue;

        // 靠海硬约束：走不到海（-1）或超出 kCoastalSteps 步一律作废换点。
        // coastalDist 为空表示本轮没圈到外海（约束自动失效），此时不做过滤。
        if (needCoastal && !coastalDist.empty())
        {
            const int steps = coastalDist[static_cast<size_t>(picked.X)
                                         + static_cast<size_t>(side) * picked.Y];
            if (steps < 1 || steps > kCoastalSteps)
                continue;
        }

        candidates.push_back(picked);
    }
    if (static_cast<int>(candidates.size()) < players)
    {
        DiagLog("SP-LAND component cells=%d only %d flat clear spots for %d players - fallback to same-level plane",
                static_cast<int>(land.size()),
                static_cast<int>(candidates.size()), players);
        return false;
    }

    // ---- 几何最远对 + 贪心最远分散（与旧版同算法，候选跨多个高程）----------
    std::vector<CellStruct> picks;
    int bi = -1, bj = -1;
    double bd = -1.0;
    for (int i = 0; i + 1 < static_cast<int>(candidates.size()); ++i)
        for (int j = i + 1; j < static_cast<int>(candidates.size()); ++j)
        {
            const int dx = candidates[i].X - candidates[j].X;
            const int dy = candidates[i].Y - candidates[j].Y;
            const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            if (d > bd) { bd = d; bi = i; bj = j; }
        }
    if (bi >= 0)
    {
        picks.push_back(candidates[bi]);
        picks.push_back(candidates[bj]);
    }
    while (static_cast<int>(picks.size()) < players)
    {
        int ci = -1;
        double bestMin = -1.0;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            double mn = 1e18;
            for (size_t j = 0; j < picks.size(); ++j)
            {
                const int dx = candidates[i].X - picks[j].X;
                const int dy = candidates[i].Y - picks[j].Y;
                const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy));
                if (d < mn) mn = d;
            }
            if (mn > bestMin) { bestMin = mn; ci = static_cast<int>(i); }
        }
        if (ci < 0) break;
        picks.push_back(candidates[ci]);
    }
    if (static_cast<int>(picks.size()) < players)
    {
        DiagLog("SP-LAND could only spread %d/%d - fallback to same-level plane",
                static_cast<int>(picks.size()), players);
        return false;
    }

    // ---- 清旧点写新点（路标索引 0..players-1 连续）------------------------
    for (size_t i = 0; i < startingPoints_.size(); ++i)
    {
        MapCell* old = CellAt(startingPoints_[i].coords.X,
                              startingPoints_[i].coords.Y);
        if (old)
            old->CellFlags &= ~4;
    }
    startingPoints_.clear();
    if (waypoints_.size() < 702)
        waypoints_.resize(702, CellStruct{ 0, 0 });
    for (int i = 0; i < players; ++i)
    {
        const CellStruct c = picks[static_cast<size_t>(i)];
        waypoints_[static_cast<size_t>(i)] = c;
        CellAt(c.X, c.Y)->CellFlags |= 4;
        startingPoints_.push_back(StartingPointRecord(i, c));
    }
    startingPointCount_ = players;

    DiagLog("SP-LAND relocated %d points across one landmass cells=%d levels=%d..%d",
            players, static_cast<int>(land.size()), loLevel, hiLevel);
    for (int i = 0; i < players; ++i)
        DiagLog("SP-LAND pt%d=(%d,%d) L%d", i, picks[i].X, picks[i].Y,
                CellAt(picks[i].X, picks[i].Y)->Level);
    return true;
}

// ---------------------------------------------------------------------------
// 大岛屿群（RmLandType::TeamContinent）专用：两个岛均分出生点。
//
// 这一型与其它地形都不同：**不做陆地连通要求**（"所有出生点彼此陆路可达"只是
// 内陆、山地、大岛屿的规则），也不要求同一高程平面。只有两条规则：
//   1. 出生点平均分摊到两块最大的陆地上（8 人 → 每岛 4 人；奇数时排前面的岛多 1）；
//   2. 沿用大岛屿的"靠海"硬约束：陆路 kCoastalSteps 步内必须能走到外海。
// 岛内仍用"几何最远对 + 贪心最远"把同岛的点拉开——那是全局偏好，不是连通规则。
//
// 两块岛用与陆路连通完全相同的陆军口径 4 向泛洪找出（可通行/沙滩、同高直通、
// 差 1 须有真坡），取格子数最大的两块；两块按外接框"先上后左"排序，排在前面的
// 岛放前半批玩家（索引小的一批）。任一步凑不齐（岛太小、找不到靠海平地）返回
// false，交调用方回退旧逻辑。
// ---------------------------------------------------------------------------
bool RandomMapGenerator::RelocateStartingPointsTwoIslands(int players)
{
    const int side = size_.workSide;
    static const int16_t kDX[4] = { 0, 1, 0, -1 };
    static const int16_t kDY[4] = { -1, 0, 1, 0 };

    const auto walkableLand = [](const MapCell* c) -> bool
    {
        return c != nullptr
            && (c->Passability == PassabilityType_Passable
                || c->Passability == PassabilityType_Beach);
    };

    // 正交邻格是否属于同一条陆军陆路（与陆路连通版同口径）。
    const auto landEdge = [&](const MapCell* a, int nx, int ny) -> bool
    {
        if (nx < 0 || ny < 0 || nx >= side || ny >= side)
            return false;
        if (!CellExists(static_cast<int16_t>(nx), static_cast<int16_t>(ny)))
            return false;
        const MapCell* b = CellAt(nx, ny);
        if (!walkableLand(b))
            return false;
        const int d = b->Level - a->Level;
        if (d == 0)
            return true;
        if (d == 1 || d == -1)
            return a->SlopeIndex != 0 || b->SlopeIndex != 0;
        return false;
    };

    // ---- 陆块 4 向泛洪 ----------------------------------------------------
    std::vector<int> compOf(static_cast<size_t>(side) * side, -1);
    std::vector<std::vector<CellStruct> > compCells;
    for (int sy = 0; sy < side; ++sy)
    {
        for (int sx = 0; sx < side; ++sx)
        {
            const int seedIdx = sx + side * sy;
            if (compOf[seedIdx] != -1)
                continue;
            if (!CellExists(static_cast<int16_t>(sx), static_cast<int16_t>(sy)))
                continue;
            const MapCell* seed = CellAt(sx, sy);
            if (!walkableLand(seed))
                continue;

            const int cid = static_cast<int>(compCells.size());
            compCells.push_back(std::vector<CellStruct>());
            std::vector<int> st;
            st.push_back(seedIdx);
            compOf[seedIdx] = cid;
            while (!st.empty())
            {
                const int idx = st.back();
                st.pop_back();
                const int px = idx % side;
                const int py = idx / side;
                compCells[cid].push_back(
                    CellStruct{ static_cast<int16_t>(px), static_cast<int16_t>(py) });
                const MapCell* cur = CellAt(px, py);
                for (int dir = 0; dir < 4; ++dir)
                {
                    const int nx = px + kDX[dir];
                    const int ny = py + kDY[dir];
                    if (nx < 0 || ny < 0 || nx >= side || ny >= side)
                        continue;
                    const int ni = nx + side * ny;
                    if (compOf[ni] != -1)
                        continue;
                    if (landEdge(cur, nx, ny))
                    {
                        compOf[ni] = cid;
                        st.push_back(ni);
                    }
                }
            }
        }
    }

    if (compCells.size() < 2)
    {
        DiagLog("SP-TEAM only %d landmass(es) - fallback",
                static_cast<int>(compCells.size()));
        return false;
    }

    // ---- 取两个最大的陆块 --------------------------------------------------
    int isl[2] = { -1, -1 };
    for (size_t i = 0; i < compCells.size(); ++i)
    {
        const int cellCount = static_cast<int>(compCells[i].size());
        if (isl[0] < 0
            || cellCount > static_cast<int>(compCells[isl[0]].size()))
        {
            isl[1] = isl[0];
            isl[0] = static_cast<int>(i);
        }
        else if (isl[1] < 0
                 || cellCount > static_cast<int>(compCells[isl[1]].size()))
        {
            isl[1] = static_cast<int>(i);
        }
    }
    if (isl[1] < 0)
    {
        DiagLog("SP-TEAM cannot find two landmasses - fallback");
        return false;
    }

    // 外接框左上角（用于给两岛定序：先上后左）。
    const auto boxMin = [&](int id) -> CellStruct
    {
        int minX = 0x7FFFFFFF;
        int minY = 0x7FFFFFFF;
        const std::vector<CellStruct>& cells = compCells[static_cast<size_t>(id)];
        for (size_t i = 0; i < cells.size(); ++i)
        {
            if (cells[i].X < minX) minX = cells[i].X;
            if (cells[i].Y < minY) minY = cells[i].Y;
        }
        return CellStruct{ static_cast<int16_t>(minX), static_cast<int16_t>(minY) };
    };
    const CellStruct top0 = boxMin(isl[0]);
    const CellStruct top1 = boxMin(isl[1]);
    if (top1.Y < top0.Y || (top1.Y == top0.Y && top1.X < top0.X))
    {
        const int swap = isl[0];
        isl[0] = isl[1];
        isl[1] = swap;
    }

    const int firstCount = (players + 1) / 2;
    const int secondCount = players - firstCount;

    const int kCliffClear = 4;
    const int visX = kVisibleOffsetX + 4;
    const int visY = kVisibleOffsetY + 4;
    const int visW = size_.width - 8;
    const int visH = size_.height - 8;

    // 靠海距离表（与大岛屿共用；为空表示本轮没圈到外海，靠海约束自动失效）。
    std::vector<int> coastalDist = BuildCoastalDistance();

    // 在某块岛内挑 count 个点：锚点纯可通行、自身平、6x6 同高净空、在可见区内、
    // 离崖够远、且靠海；再按几何最远对 + 贪心最远拉开。点位只与本岛已选点比较，
    // 不会跑到另一块岛那一侧去。
    const auto spreadOnIsland =
        [&](const std::vector<CellStruct>& island, int count,
            std::vector<CellStruct>& out) -> bool
    {
        std::vector<CellStruct> candidates;
        candidates.reserve(island.size());
        for (size_t i = 0; i < island.size(); ++i)
        {
            const CellStruct picked = island[i];
            const MapCell* c = CellAt(picked.X, picked.Y);
            if (c->Passability != PassabilityType_Passable || c->SlopeIndex != 0)
                continue;
            if (!TileRectLevelClear(picked.X - 3, picked.Y - 3, 6, 6, false))
                continue;
            if (!IsWithinUsableRect(picked, true, visX, visY, visW, visH))
                continue;
            if (!IsCliffClearAround(picked, kCliffClear))
                continue;
            if (!coastalDist.empty())
            {
                const int steps =
                    coastalDist[static_cast<size_t>(picked.X)
                                + static_cast<size_t>(side) * picked.Y];
                if (steps < 1 || steps > kCoastalSteps)
                    continue;
            }
            candidates.push_back(picked);
        }
        if (static_cast<int>(candidates.size()) < count)
            return false;

        std::vector<CellStruct> local;
        if (count >= 2)
        {
            int bi = -1, bj = -1;
            double bd = -1.0;
            for (int i = 0; i + 1 < static_cast<int>(candidates.size()); ++i)
                for (int j = i + 1; j < static_cast<int>(candidates.size()); ++j)
                {
                    const int dx = candidates[i].X - candidates[j].X;
                    const int dy = candidates[i].Y - candidates[j].Y;
                    const double d =
                        std::sqrt(static_cast<double>(dx * dx + dy * dy));
                    if (d > bd) { bd = d; bi = i; bj = j; }
                }
            local.push_back(candidates[bi]);
            local.push_back(candidates[bj]);
        }
        else
        {
            local.push_back(candidates[0]);
        }
        while (static_cast<int>(local.size()) < count)
        {
            int ci = -1;
            double bestMin = -1.0;
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                double mn = 1e18;
                for (size_t j = 0; j < local.size(); ++j)
                {
                    const int dx = candidates[i].X - local[j].X;
                    const int dy = candidates[i].Y - local[j].Y;
                    const double d =
                        std::sqrt(static_cast<double>(dx * dx + dy * dy));
                    if (d < mn) mn = d;
                }
                if (mn > bestMin) { bestMin = mn; ci = static_cast<int>(i); }
            }
            if (ci < 0)
                break;
            local.push_back(candidates[ci]);
        }
        if (static_cast<int>(local.size()) < count)
            return false;

        out.insert(out.end(), local.begin(), local.end());
        return true;
    };

    std::vector<CellStruct> picks;
    if (!spreadOnIsland(compCells[static_cast<size_t>(isl[0])], firstCount, picks)
        || !spreadOnIsland(compCells[static_cast<size_t>(isl[1])], secondCount,
                           picks))
    {
        DiagLog("SP-TEAM islands cells=%d/%d cannot host %d+%d players - fallback",
                static_cast<int>(compCells[isl[0]].size()),
                static_cast<int>(compCells[isl[1]].size()),
                firstCount, secondCount);
        return false;
    }

    // ---- 清旧点写新点（路标索引 0..players-1 连续）------------------------
    for (size_t i = 0; i < startingPoints_.size(); ++i)
    {
        MapCell* old = CellAt(startingPoints_[i].coords.X,
                              startingPoints_[i].coords.Y);
        if (old)
            old->CellFlags &= ~4;
    }
    startingPoints_.clear();
    if (waypoints_.size() < 702)
        waypoints_.resize(702, CellStruct{ 0, 0 });
    for (int i = 0; i < players; ++i)
    {
        const CellStruct c = picks[static_cast<size_t>(i)];
        waypoints_[static_cast<size_t>(i)] = c;
        CellAt(c.X, c.Y)->CellFlags |= 4;
        startingPoints_.push_back(StartingPointRecord(i, c));
    }
    startingPointCount_ = players;

    DiagLog("SP-TEAM split %d points over two landmasses (%d+%d) cells=%d/%d",
            players, firstCount, secondCount,
            static_cast<int>(compCells[isl[0]].size()),
            static_cast<int>(compCells[isl[1]].size()));
    for (int i = 0; i < players; ++i)
        DiagLog("SP-TEAM pt%d=(%d,%d) island=%d", i, picks[i].X, picks[i].Y,
                (i < firstCount ? 0 : 1));
    return true;
}

// ---------------------------------------------------------------------------
// RelocateStartingPointsOneIslandEach [port-only]
//
// 群岛（RmLandType::Archipelago）专用：出生点改成"一岛一点"。
//
//   * 陆地按陆军口径 4 向泛洪成一块块岛，按格子数从大到小取前 min(玩家人数,
//     岛数) 块，每块放**恰好一个**出生点；岛数多于玩家人数时多出来的不管。
//   * 不做陆地连通要求、不要求同一高程平面、**不要求靠海**。
//   * 岛内锚点沿用各路径共用口径（可通行 + 自身平 + 6x6 同高净空 + 在可见区内
//     + 离崖 4 格）；某岛严格口径找不到时依次放宽到"可通行 + 自身平 + 可见区
//     内"、再到"可通行"，尽量保证每岛都有点。
//   * 岛数少于玩家人数时凑不齐，返回 false 交调用方回退旧逻辑。
// ---------------------------------------------------------------------------
bool RandomMapGenerator::RelocateStartingPointsOneIslandEach(int players)
{
    const int side = size_.workSide;
    static const int16_t kDX[4] = { 0, 1, 0, -1 };
    static const int16_t kDY[4] = { -1, 0, 1, 0 };

    const auto walkableLand = [](const MapCell* c) -> bool
    {
        return c != nullptr
            && (c->Passability == PassabilityType_Passable
                || c->Passability == PassabilityType_Beach);
    };

    // 正交邻格是否属于同一条陆军陆路（与陆路连通版同口径）。
    const auto landEdge = [&](const MapCell* a, int nx, int ny) -> bool
    {
        if (nx < 0 || ny < 0 || nx >= side || ny >= side)
            return false;
        if (!CellExists(static_cast<int16_t>(nx), static_cast<int16_t>(ny)))
            return false;
        const MapCell* b = CellAt(nx, ny);
        if (!walkableLand(b))
            return false;
        const int d = b->Level - a->Level;
        if (d == 0)
            return true;
        if (d == 1 || d == -1)
            return a->SlopeIndex != 0 || b->SlopeIndex != 0;
        return false;
    };

    // ---- 陆块 4 向泛洪 ----------------------------------------------------
    std::vector<int> compOf(static_cast<size_t>(side) * side, -1);
    std::vector<std::vector<CellStruct> > compCells;
    for (int sy = 0; sy < side; ++sy)
    {
        for (int sx = 0; sx < side; ++sx)
        {
            const int seedIdx = sx + side * sy;
            if (compOf[seedIdx] != -1)
                continue;
            if (!CellExists(static_cast<int16_t>(sx), static_cast<int16_t>(sy)))
                continue;
            const MapCell* seed = CellAt(sx, sy);
            if (!walkableLand(seed))
                continue;

            const int cid = static_cast<int>(compCells.size());
            compCells.push_back(std::vector<CellStruct>());
            std::vector<int> st;
            st.push_back(seedIdx);
            compOf[seedIdx] = cid;
            while (!st.empty())
            {
                const int idx = st.back();
                st.pop_back();
                const int px = idx % side;
                const int py = idx / side;
                compCells[cid].push_back(
                    CellStruct{ static_cast<int16_t>(px), static_cast<int16_t>(py) });
                const MapCell* cur = CellAt(px, py);
                for (int dir = 0; dir < 4; ++dir)
                {
                    const int nx = px + kDX[dir];
                    const int ny = py + kDY[dir];
                    if (nx < 0 || ny < 0 || nx >= side || ny >= side)
                        continue;
                    const int ni = nx + side * ny;
                    if (compOf[ni] != -1)
                        continue;
                    if (landEdge(cur, nx, ny))
                    {
                        compOf[ni] = cid;
                        st.push_back(ni);
                    }
                }
            }
        }
    }

    if (static_cast<int>(compCells.size()) < players)
    {
        DiagLog("SP-ARCH only %d landmass(es) < %d players - fallback",
                static_cast<int>(compCells.size()), players);
        return false;
    }

    // ---- 按格子数从大到小挑出前 players 块岛 ------------------------------
    std::vector<char> used(compCells.size(), 0);
    std::vector<int> order;
    order.reserve(static_cast<size_t>(players));
    for (int k = 0; k < players; ++k)
    {
        int best = -1;
        for (size_t i = 0; i < compCells.size(); ++i)
        {
            if (used[i])
                continue;
            if (best < 0
                || compCells[i].size() > compCells[static_cast<size_t>(best)].size())
            {
                best = static_cast<int>(i);
            }
        }
        used[static_cast<size_t>(best)] = 1;
        order.push_back(best);
    }

    const int kCliffClear = 4;
    const int visX = kVisibleOffsetX + 4;
    const int visY = kVisibleOffsetY + 4;
    const int visW = size_.width - 8;
    const int visH = size_.height - 8;

    const auto commonAnchor = [&](CellStruct picked) -> bool
    {
        const MapCell* c = CellAt(picked.X, picked.Y);
        return c->Passability == PassabilityType_Passable
            && c->SlopeIndex == 0
            && TileRectLevelClear(picked.X - 3, picked.Y - 3, 6, 6, false)
            && IsWithinUsableRect(picked, true, visX, visY, visW, visH)
            && IsCliffClearAround(picked, kCliffClear);
    };

    // ---- 每岛恰好一个点 ----------------------------------------------------
    std::vector<CellStruct> picks;
    picks.reserve(static_cast<size_t>(players));
    for (int k = 0; k < players; ++k)
    {
        const std::vector<CellStruct>& island =
            compCells[static_cast<size_t>(order[static_cast<size_t>(k)])];

        CellStruct chosen = island[0];
        bool found = false;

        for (size_t i = 0; i < island.size() && !found; ++i)
        {
            if (commonAnchor(island[i]))
            {
                chosen = island[i];
                found = true;
            }
        }
        for (size_t i = 0; i < island.size() && !found; ++i)
        {
            // 放宽一档：可通行 + 自身平 + 在可见区内。
            const MapCell* c = CellAt(island[i].X, island[i].Y);
            if (c->Passability == PassabilityType_Passable && c->SlopeIndex == 0
                && IsWithinUsableRect(island[i], true, visX, visY, visW, visH))
            {
                chosen = island[i];
                found = true;
            }
        }
        for (size_t i = 0; i < island.size() && !found; ++i)
        {
            // 再放宽一档：只要是能站人的陆地格。
            const MapCell* c = CellAt(island[i].X, island[i].Y);
            if (c->Passability == PassabilityType_Passable)
            {
                chosen = island[i];
                found = true;
            }
        }
        if (!found)
        {
            DiagLog("SP-ARCH island %d cells=%d has no anchor - fallback",
                    order[static_cast<size_t>(k)],
                    static_cast<int>(island.size()));
            return false;
        }
        picks.push_back(chosen);
    }

    // ---- 清旧点写新点（路标索引 0..players-1 连续）------------------------
    for (size_t i = 0; i < startingPoints_.size(); ++i)
    {
        MapCell* old = CellAt(startingPoints_[i].coords.X,
                              startingPoints_[i].coords.Y);
        if (old)
            old->CellFlags &= ~4;
    }
    startingPoints_.clear();
    if (waypoints_.size() < 702)
        waypoints_.resize(702, CellStruct{ 0, 0 });
    for (int i = 0; i < players; ++i)
    {
        const CellStruct c = picks[static_cast<size_t>(i)];
        waypoints_[static_cast<size_t>(i)] = c;
        CellAt(c.X, c.Y)->CellFlags |= 4;
        startingPoints_.push_back(StartingPointRecord(i, c));
    }
    startingPointCount_ = players;

    DiagLog("SP-ARCH one point per island: %d points over %d landmasses",
            players, static_cast<int>(compCells.size()));
    for (int i = 0; i < players; ++i)
        DiagLog("SP-ARCH pt%d=(%d,%d) island=%d", i, picks[i].X, picks[i].Y,
                order[static_cast<size_t>(i)]);
    return true;
}

// 出生点阶段内部的连通性校正：把所有出生点收拢到最大的同高可行走平面。
void RandomMapGenerator::EnsureStartingPointsConnected()
{
    const int players = config_.playerCount;
    if (players <= 1 || static_cast<int>(startingPoints_.size()) <= 1)
        return;   // 0/1 个点谈不上互相连通

    // [port-only] 群岛（Archipelago）单独一套：一岛一点 —— 取最大的 min(玩家人数,
    // 岛数) 块陆岛各放一个点。不要求靠海、不做陆地连通/同高平面要求。失败时回退。
    const int landType = static_cast<int>(config_.landType);
    if (landType == static_cast<int>(RmLandType::Archipelago)
        && RelocateStartingPointsOneIslandEach(players))
    {
        return;
    }

    // [port-only] 大岛屿群（TeamContinent）单独一套：出生点平均分摊到两块最大的
    // 陆地上，**不做陆地连通要求**（"彼此陆路可达"只是内陆/山地/大岛屿的规则），
    // 也不要求同一高程平面；只保留与大岛屿共有的靠海约束。失败时回退旧逻辑。
    if (landType == static_cast<int>(RmLandType::TeamContinent)
        && RelocateStartingPointsTwoIslands(players))
    {
        return;
    }

    // [port-only] 三种陆地型（大岛屿/内陆/山地）：出生点不强制在同一高程平面
    // 上，只要彼此可经真实斜坡陆路连通即可（可一个在崖上一个在崖下）。群岛保持
    // 旧的"同一同高平面"机制不变。新版失败时（极小概率：一个陆块里实在凑不齐
    // 全部玩家的 6x6 平地）回退旧逻辑，绝不比旧版差。
    if ((landType == static_cast<int>(RmLandType::Continent)
         || landType == static_cast<int>(RmLandType::Inland)
         || landType == static_cast<int>(RmLandType::Mountainous))
        && RelocateStartingPointsLandConnected(players))
    {
        return;
    }

    const int side = size_.workSide;
    static const int16_t kDX[4] = { 0, 1, 0, -1 };
    static const int16_t kDY[4] = { -1, 0, 1, 0 };

    // ---- 同高正交连通分块 ------------------------------------------------
    std::vector<int> blockOf(static_cast<size_t>(side) * side, -1);
    std::vector<std::vector<CellStruct> > blockCells;
    for (int sy = 0; sy < side; ++sy)
    {
        for (int sx = 0; sx < side; ++sx)
        {
            const int seedIdx = sx + side * sy;
            if (blockOf[seedIdx] != -1)
                continue;
            if (!CellExists(static_cast<int16_t>(sx), static_cast<int16_t>(sy)))
                continue;
            const MapCell* seed = CellAt(sx, sy);
            if (seed->Passability != PassabilityType_Passable)
                continue;

            const int level = seed->Level;
            const int bid = static_cast<int>(blockCells.size());
            blockCells.push_back(std::vector<CellStruct>());
            std::vector<int> st;
            st.push_back(seedIdx);
            blockOf[seedIdx] = bid;
            while (!st.empty())
            {
                const int idx = st.back();
                st.pop_back();
                const int px = idx % side;
                const int py = idx / side;
                blockCells[bid].push_back(
                    CellStruct{ static_cast<int16_t>(px), static_cast<int16_t>(py) });
                for (int d = 0; d < 4; ++d)
                {
                    const int nx = px + kDX[d];
                    const int ny = py + kDY[d];
                    if (nx < 0 || ny < 0 || nx >= side || ny >= side)
                        continue;
                    const int ni = nx + side * ny;
                    if (blockOf[ni] != -1)
                        continue;
                    if (!CellExists(static_cast<int16_t>(nx), static_cast<int16_t>(ny)))
                        continue;
                    const MapCell* nc = CellAt(nx, ny);
                    if (nc->Passability != PassabilityType_Passable
                        || nc->Level != level)
                        continue;
                    blockOf[ni] = bid;
                    st.push_back(ni);
                }
            }
        }
    }

    // ---- 记录原点是否已连通（仅用于日志）；无论是否连通都要重选以拉远距离 --
    int commonBlock = -1;
    bool allSame = true;
    for (size_t i = 0; i < startingPoints_.size(); ++i)
    {
        const CellStruct c = startingPoints_[i].coords;
        if (!CellExists(c.X, c.Y))
        {
            allSame = false;
            break;
        }
        const int b = blockOf[c.X + side * c.Y];
        if (b < 0)
        {
            allSame = false;
            break;
        }
        if (commonBlock < 0)
            commonBlock = b;
        else if (b != commonBlock)
            allSame = false;
    }
    DiagLog("SP-CONNECT original points connected=%s (block cells=%d) - spreading to maximize distance",
            allSame ? "YES" : "NO",
            (commonBlock >= 0 ? static_cast<int>(blockCells[commonBlock].size()) : 0));

    // ---- 选定用来放全部玩家的连通平面 ------------------------------------
    // 优先锁定"原点已经共同所在的那个平面"：只在其内把点拉到最远，绝不把玩家
    // 搬到另一块（哪怕更大的）台地——否则会改变出生地形、且新平面未必安全。
    // 只有当原点分散在不同平面（不连通）时，才回退到能容纳全部玩家的最大平面。
    int best = -1;
    if (allSame && commonBlock >= 0
        && static_cast<int>(blockCells[commonBlock].size()) >= players)
    {
        best = commonBlock;
    }
    else
    {
        for (size_t b = 0; b < blockCells.size(); ++b)
            if (static_cast<int>(blockCells[b].size()) >= players
                && (best < 0
                    || static_cast<int>(blockCells[b].size())
                       > static_cast<int>(blockCells[best].size())))
                best = static_cast<int>(b);
    }
    if (best < 0)
    {
        DiagLog("SP-CONNECT no single same-level plane holds %d players - leaving points as-is",
                players);
        return;
    }

    const std::vector<CellStruct>& plane = blockCells[best];
    const int planeLevel = CellAt(plane[0].X, plane[0].Y)->Level;

    // ---- 枚举平面内全部 6×6 净地格作为候选（不随机限量）------------------
    // 连通是硬约束，在此前提下要让玩家点尽量远，就必须在整块连通平面的所有能放
    // 建筑的格子里挑几何最远的，而不是只在一小撮随机样本里挑（那样抽不到平面
    // 两端，点会挤在一起）。
    const int visX = kVisibleOffsetX + 4;
    const int visY = kVisibleOffsetY + 4;
    const int visW = size_.width - 8;
    const int visH = size_.height - 8;
    // 与陆路版同一圈悬崖缓冲，保证回退到同高平面时也不会把点放到崖边。
    const int kCliffClear = 4;
    // 大岛屿 + 大岛屿群硬约束在回退路径上同样生效：点必须靠海。
    const bool needCoastal = (config_.landType == RmLandType::Continent
                              || config_.landType == RmLandType::TeamContinent);
    std::vector<int> coastalDist;
    if (needCoastal)
        coastalDist = BuildCoastalDistance();
    std::vector<CellStruct> candidates;
    const int cellCount = static_cast<int>(plane.size());
    candidates.reserve(static_cast<size_t>(cellCount));
    for (int i = 0; i < cellCount; ++i)
    {
        const CellStruct picked = plane[static_cast<size_t>(i)];
        if (!TileRectClear(picked.X - 3, picked.Y - 3, 6, 6, false, false))
            continue;
        if (!IsWithinUsableRect(picked, true, visX, visY, visW, visH))
            continue;
        if (!IsCliffClearAround(picked, kCliffClear))
            continue;
        // 靠海硬约束（大岛屿）：走不到海或超出 kCoastalSteps 步一律作废。
        // coastalDist 为空表示本轮没圈到外海（约束自动失效），此时不做过滤。
        if (needCoastal && !coastalDist.empty())
        {
            const int steps = coastalDist[static_cast<size_t>(picked.X)
                                         + static_cast<size_t>(side) * picked.Y];
            if (steps < 1 || steps > kCoastalSteps)
                continue;
        }
        candidates.push_back(picked);
    }
    if (static_cast<int>(candidates.size()) < players)
    {
        DiagLog("SP-CONNECT plane L%d cells=%d only %d clear spots - leaving points as-is",
                planeLevel, cellCount, static_cast<int>(candidates.size()));
        return;
    }

    // ---- 最远分散选 players 个 -------------------------------------------
    std::vector<CellStruct> picks;
    int bi = -1, bj = -1;
    double bd = -1.0;
    for (int i = 0; i + 1 < static_cast<int>(candidates.size()); ++i)
        for (int j = i + 1; j < static_cast<int>(candidates.size()); ++j)
        {
            const int dx = candidates[i].X - candidates[j].X;
            const int dy = candidates[i].Y - candidates[j].Y;
            const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            if (d > bd) { bd = d; bi = i; bj = j; }
        }
    if (bi >= 0)
    {
        picks.push_back(candidates[bi]);
        picks.push_back(candidates[bj]);
    }
    while (static_cast<int>(picks.size()) < players)
    {
        int ci = -1;
        double bestMin = -1.0;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            double mn = 1e18;
            for (size_t j = 0; j < picks.size(); ++j)
            {
                const int dx = candidates[i].X - picks[j].X;
                const int dy = candidates[i].Y - picks[j].Y;
                const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy));
                if (d < mn) mn = d;
            }
            if (mn > bestMin) { bestMin = mn; ci = static_cast<int>(i); }
        }
        if (ci < 0) break;
        picks.push_back(candidates[ci]);
    }
    if (static_cast<int>(picks.size()) < players)
    {
        DiagLog("SP-CONNECT could only spread %d/%d on plane L%d - leaving points as-is",
                static_cast<int>(picks.size()), players, planeLevel);
        return;
    }

    // ---- 清旧点写新点（路标索引 0..players-1 连续）-----------------------
    for (size_t i = 0; i < startingPoints_.size(); ++i)
    {
        MapCell* old = CellAt(startingPoints_[i].coords.X,
                              startingPoints_[i].coords.Y);
        if (old)
            old->CellFlags &= ~4;
    }
    startingPoints_.clear();
    if (waypoints_.size() < 702)
        waypoints_.resize(702, CellStruct{ 0, 0 });
    for (int i = 0; i < players; ++i)
    {
        const CellStruct c = picks[static_cast<size_t>(i)];
        waypoints_[static_cast<size_t>(i)] = c;
        CellAt(c.X, c.Y)->CellFlags |= 4;
        startingPoints_.push_back(StartingPointRecord(i, c));
    }
    startingPointCount_ = players;

    DiagLog("SP-CONNECT relocated %d points onto one same-level plane L%d cells=%d",
            players, planeLevel, cellCount);
    for (int i = 0; i < players; ++i)
        DiagLog("SP-CONNECT pt%d=(%d,%d)", i, picks[i].X, picks[i].Y);
}

// ---------------------------------------------------------------------------
// sub_594420 (0x594420 - 0x59486F) - grow one region out of a seed cell.
//
// Full body in 594420.c (decompile folder). In short:
//
//   region = CreateRegionRecord(seed coords)   // sub_58BF70, id = ABED14++
//   mark   = region->id                       // the id is read back at +0x08
//   work[seed].data[15] = mark                // 0x594498
//   list   = { seed coords }
//   while (list not empty)
//   {
//       cur = list.pop_back()                 // pops the LAST entry -> a DFS
//       work[cur].data[14] = mark             // 0x59454a
//       ++region->cellCount                   // 0x594552
//       region->cells.push(cur coords)        // 0x59458b
//       for (dir = 0 .. 7)                    // Neighbours order, 16-bit adds
//       {
//           n = cur + Neighbours[dir]
//           if (!CellExists(n)) continue;
//           MapCell* nb = CellAt(n);
//           if (nb->Passability != Passable) continue;
//           if (GetMoveError(n, zone) != refErr) continue;
//           if (work[n].data[15] != -1) continue;   // 0x594664
//           work[n].data[15] = mark;                // 0x59466a, on discovery
//           list.push(n);
//       }
//   }
//
// The mark is stamped into data[15] on DISCOVERY and into data[14] on POP, so a
// cell that is already queued is never queued twice. No RNG is consumed.
// ---------------------------------------------------------------------------
void RandomMapGenerator::SeedRegionFromCell(MapCell* seedCell, int movementZone,
                                            int referenceError)
{
    if (seedCell == nullptr || workCells_ == nullptr)
        return;

    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    const int side = size_.workSide;

    MapRegion* region = CreateRegionRecord(seedCell->MapCoords);   // 0x59442e
    const int mark = region->id;                                   // 0x59444b

    const int seedPacked = seedCell->MapCoords;
    const int16_t seedX = static_cast<int16_t>(seedPacked & 0xFFFF);
    const int16_t seedY = static_cast<int16_t>((uint32_t)seedPacked >> 16);

    workCells_[seedX + side * seedY].data[15] = mark;              // 0x594498

    std::vector<CellStruct> pending;
    pending.push_back(CellStruct{ seedX, seedY });                 // 0x5944e0

    // 连通判据严格沿用游戏自己的移动分量（与原版 sub_594420 完全一致）：8 邻域、
    // Passable 且 GetMoveError 等于参考分量。放宽重圈只降低后续的区域大小门槛，
    // 绝不改变连通判据——否则会把游戏单位实际走不通的相邻格误判成连通。
    while (!pending.empty())
    {
        const CellStruct cur = pending.back();                     // 0x594503
        pending.pop_back();

        workCells_[cur.X + side * cur.Y].data[14] = mark;          // 0x59454a
        ++region->cellCount;                                       // 0x594552
        region->cells.push_back(cur);                              // 0x59458b

        for (int dir = 0; dir < 8; ++dir)                          // 0x594598
        {
            const int16_t nx = static_cast<int16_t>(cur.X + kDirX[dir]);
            const int16_t ny = static_cast<int16_t>(cur.Y + kDirY[dir]);
            if (!CellExists(nx, ny))
                continue;

            MapCell* neighbour = CellAt(nx, ny);
            if (neighbour->Passability != PassabilityType_Passable)
                continue;
            if (GetMoveError(CellStruct{ nx, ny }, movementZone) != referenceError)
                continue;

            WorkCell& nb = workCells_[nx + side * ny];
            if (nb.data[15] != -1)                                 // 0x594664
                continue;

            nb.data[15] = mark;                                    // 0x59466a
            pending.push_back(CellStruct{ nx, ny });               // 0x5946b0
        }
    }
}

// ---------------------------------------------------------------------------
// sub_594870 (0x594870 - 0x594A55) - stamp one region's starting-point cells.
//
// Full body in 594870.c (decompile folder); the helper it calls, sub_594F40,
// chooses WHICH candidates become starting points.
//
//   1. Sample candidates (0x594898 - 0x594a1b). The usable box is the
//      scenario's LocalSize rect inflated by (4, 4, -8, -8). One draw gives
//          do { n2 = F2I64(Random() * 3 * kUnitScale); } while (n2 > 2);
//      and the loop then tries to collect `n2 + 15 * region->startingPoints`
//      candidates - at most 300 tries - where each try draws one index into the
//      region's cell list,
//          do { i = F2I64(Random() * cellCount * kUnitScale); } while (i > n-1);
//      and keeps that cell when both tests pass:
//          TileRectClear(x - 3, y - 3, 6, 6, false, false)     (sub_5A7250)
//          && IsWithinUsableRect(cell, true, the inflated rect) (sub_578640)
//      Two RNG draws per try, and the cell list is never consumed, so one cell
//      can be drawn (and kept) more than once.
//
//   2. Choose the picks (sub_594F40, no RNG - see ChooseStartingPointCells).
//
//   3. Stamp (0x594a5e - 0x594ae0): for i in 0 .. points-1,
//          waypoints_[offset + i] = picks[i]                   (sub_68BF50)
//          the cell at picks[i] -> CellFlags |= 4               (0x594ac1)
//      then drop those `points` entries off the FRONT of the pick list and keep
//      the rest on the region as MapRegion::startPointPicks (the vanilla's
//      region +0x00).
//
// The vanilla also writes the first eight picks into the eight dwords at
// ScenarioClass+0x11C0 (after filling them with dword_ABE300 = 0) - the port
// does not carry that array, see the note in MapGen.h.
//
// Deviation: when region->startingPoints is larger than the pick list the
// vanilla's stamp loop reads past the end of the list. The port stops at the
// end instead of reading heap garbage.
// ---------------------------------------------------------------------------
void RandomMapGenerator::StampStartingPointCells(MapRegion* region, int offset)
{
    if (region == nullptr)
        return;

    const int points = region->startingPoints;      // region +0x20
    const int cellCount = static_cast<int>(region->cells.size());

    // A region always carries at least the cell it was seeded from, so this is
    // only here because the vanilla would spin forever on an empty list.
    if (cellCount == 0)
        return;

    // ---- 1. sample the candidates (0x594898 - 0x594a1b) -------------------
    const int visX = kVisibleOffsetX + 4;
    const int visY = kVisibleOffsetY + 4;
    const int visW = size_.width - 8;
    const int visH = size_.height - 8;

    int n2;
    do
    {
        n2 = F2I64(static_cast<double>(rng_.Next()) * (3.0 * kUnitScale));
    }
    while (n2 > 2);

    const int wanted = n2 + 15 * points;            // 12 * points + 3 * points

    std::vector<CellStruct> candidates;
    int rejectRect = 0;
    int rejectBand = 0;
    for (int attempt = 0; static_cast<int>(candidates.size()) < wanted; ++attempt)
    {
        if (attempt >= 300)                         // 0x594918
            break;

        int index;
        do
        {
            index = F2I64(static_cast<double>(rng_.Next())
                          * static_cast<double>(cellCount) * kUnitScale);
        }
        while (index > cellCount - 1);

        const CellStruct picked = region->cells[static_cast<size_t>(index)];
        if (!TileRectClear(picked.X - 3, picked.Y - 3, 6, 6, false, false))
        {
            ++rejectRect;
            continue;
        }
        if (!IsWithinUsableRect(picked, true, visX, visY, visW, visH))
        {
            ++rejectBand;
            continue;
        }

        candidates.push_back(picked);               // 0x5949fb
    }
    if (points > 0 && static_cast<int>(candidates.size()) < wanted)
        DiagLog("SP-SAMPLE region=%d tries ran out: rejected by 6x6 TileRectClear=%d, by usable band=%d",
                region->id, rejectRect, rejectBand);

    // ---- 2. choose --------------------------------------------------------
    const std::vector<CellStruct> picks = ChooseStartingPointCells(region,
                                                                   candidates);
    DiagLog("SP-STAMP region=%d points=%d cellCount=%d wanted=%d candidates=%d picks=%d",
            region->id, points, cellCount, wanted,
            static_cast<int>(candidates.size()), static_cast<int>(picks.size()));
    if (picks.empty() && points > 0)
    {
        // The vanilla keeps going here and stamps garbage; with nothing to
        // stamp there is nothing to record either.
        region->startPointPicks = picks;
        return;
    }

    // ---- 3. stamp (0x594a5e - 0x594ac1) -----------------------------------
    const int stampable = (points < static_cast<int>(picks.size()))
                            ? points : static_cast<int>(picks.size());
    DiagLog("SP-STAMP region=%d stamped=%d (budget points=%d)",
            region->id, stampable, points);
    for (int i = 0; i < stampable; ++i)
    {
        const CellStruct cell = picks[static_cast<size_t>(i)];
        const int index = offset + i;

        if (index >= 0 && index < static_cast<int>(waypoints_.size()))
            waypoints_[static_cast<size_t>(index)] = cell;   // sub_68BF50

        MapCell* mapCell = CellAt(cell.X, cell.Y);
        mapCell->CellFlags |= 4;                             // 0x594ac1

        startingPoints_.push_back(StartingPointRecord(index, cell));
    }

    // The leftovers stay on the region (the vanilla's region +0x00).
    std::vector<CellStruct> rest(picks.begin() + stampable, picks.end());
    region->startPointPicks.swap(rest);
}

// ---------------------------------------------------------------------------
// sub_594F40 (0x594F40 - 0x5953F3) - which of the candidates become starting
// points.
//
// Full body in 594870.c. In short:
//
//   points = region->startingPoints
//   want   = F2I64((dword_ABE030 * 0.01 * 12.0 / dword_ABE028 + 2.0) * points)
//            - dword_ABE030 is 0 (its only xref is the read here) and
//              dword_ABE028 is the budget 4, so this is 2 * points;
//   limit  = want, capped at the candidate count. When points == 0 the whole
//            candidate list is used instead (and a list that is empty as well
//            answers an empty pick list).
//
//   First pick: over every pair (i, j), j > i, of candidates compute
//       d = sqrt(dx^2 + dy^2) + (mark_i != mark_j ? 20 : 0)
//   where the marks are the cells' work[+14] region marks, and keep the i of
//   the largest d (strict >, so the first maximum wins). That cell is the one
//   whose farthest partner is farthest.
//
//   Then fill: while (picks < limit) append the candidate whose MINIMUM distance
//   to the already picked cells is the largest (same distance formula). The pool
//   is not consumed, but a picked cell sits at distance 0 from itself once the
//   pick list is non-empty, so nothing is ever picked twice.
//
//   The comparison seeds are -1.0 for the first phase and 9999999.0 for the
//   inner minimum of the second, exactly as the vanilla.
//
// No RNG is consumed.
// ---------------------------------------------------------------------------
std::vector<CellStruct> RandomMapGenerator::ChooseStartingPointCells(
        const MapRegion* region, const std::vector<CellStruct>& candidates)
{
    std::vector<CellStruct> picks;
    if (region == nullptr)
        return picks;

    const int points = region->startingPoints;
    const int candidateCount = static_cast<int>(candidates.size());

    // dword_ABE030 = the rolled TiberiumLayout option and dword_ABE028 = the
    // player count; the vanilla's shape stays visible.
    const double wantDouble =
        (static_cast<double>(globalOptions_.tiberiumLayout) * 0.01 * 12.0
         / static_cast<double>(config_.playerCount) + 2.0)
        * static_cast<double>(points);
    int want = F2I64(wantDouble);

    int limit = want;                                       // vanilla's v36
    if (points == 0 && candidateCount > 0)
    {
        limit = candidateCount;
        want = limit;
    }
    if (want > candidateCount || want == 0)
    {
        if (points == 0)
            return picks;
        limit = candidateCount;
    }

    const int side = size_.workSide;

    // The cell's work[+14] region mark, -1 when the work array is missing (the
    // vanilla's `v9 ? ... : -1`).
    const std::vector<CellStruct>& pool = candidates;

    // ---- the first pick (0x595002 - 0x595198) -----------------------------
    if (candidateCount > 1 && limit > 0)
    {
        double best = -1.0;
        int bestIndex = -1;
        for (int i = 0; i + 1 < candidateCount; ++i)
        {
            const CellStruct a = pool[static_cast<size_t>(i)];
            const int markA = workCells_[a.X + side * a.Y].data[14];
            for (int j = i + 1; j < candidateCount; ++j)
            {
                const CellStruct b = pool[static_cast<size_t>(j)];
                const int markB = workCells_[b.X + side * b.Y].data[14];
                const int dx = a.X - b.X;
                const int dy = a.Y - b.Y;
                const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy))
                               + (markA != markB ? 20 : 0);
                if (j != 0 && d > best)                     // 0x5950fd
                {
                    best = d;
                    bestIndex = i;
                }
            }
        }
        if (bestIndex != -1)
            picks.push_back(pool[static_cast<size_t>(bestIndex)]);
    }

    // ---- the fill (0x595198 - 0x5953f3) -----------------------------------
    while (static_cast<int>(picks.size()) < limit)
    {
        double best = -1.0;
        int bestIndex = -1;
        for (int i = 0; i < candidateCount; ++i)
        {
            const CellStruct a = pool[static_cast<size_t>(i)];
            const int markA = workCells_[a.X + side * a.Y].data[14];

            double nearest = 9999999.0;
            for (size_t j = 0; j < picks.size(); ++j)
            {
                const CellStruct b = picks[j];
                const int markB = workCells_[b.X + side * b.Y].data[14];
                const int dx = a.X - b.X;
                const int dy = a.Y - b.Y;
                const double d =
                    std::sqrt(static_cast<double>(dx * dx + dy * dy))
                    + (markA != markB ? 20 : 0);
                if (d < nearest)
                    nearest = d;
            }

            if (nearest > best)
            {
                best = nearest;
                bestIndex = i;
            }
        }

        if (bestIndex == -1)
            break;                                          // no candidates left
        picks.push_back(pool[static_cast<size_t>(bestIndex)]);
    }

    return picks;
}

// ---------------------------------------------------------------------------
// Locate one of the game's INI files: next to the executable first, then one and
// two levels up, then the project's rules-INI folder - the same candidate list
// LoadOverlayTypes (MapGenRecalc.cpp) uses. False when none of them exists.
// ---------------------------------------------------------------------------
static bool FindGameIni(const wchar_t* fileName, char* out, int outSize)
{
    // [FA2sp] One resolver for every INI the generator reads - see
    // RandomMapGenerator::ResolveGameIni.
    wchar_t path[MAX_PATH] = L"";
    if (!RandomMapGenerator::ResolveGameIni(fileName, path, MAX_PATH))
        return false;
    WideCharToMultiByte(CP_ACP, 0, path, -1, out, outSize, nullptr, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// LoadNeutralTechBuildings - RulesClass::NeutralTechBuildings (rules INI
// [AI]) plus each type's foundation.
//
// The rules key is a comma-separated list of [BuildingTypes] names. NOTE the
// section: although it is a RulesClass member, Westwood reads this list from
// the [AI] section, not [General] - the shipped rulesmd.ini (and D:\Ra2's)
// carries the only copy at [AI] NeutralTechBuildings, with no General-side key
// at all. Reading it from [General] answers the empty default and the tech
// stage silently places nothing.
//
// The size comes from the ART INI, because that is where
// BuildingTypeClass::LoadFromINI
// reads "Foundation" (INI_Art, 0x461225 - 0x46125d, twice: once from the
// entry's own section and once from the section its Image= names, keeping
// whichever answered). The value is a name from the 22-entry table at 0x81B9D8
// ("1x1" .. "3x3Refinery" .. "6x4", plus a final "0x0"), which pairs one to one
// with the width / height tables at 0x8192B8 / 0x819310; the port only needs
// the two numbers, so it parses "WxH" directly.
//
// A type whose art has no Foundation= keeps 1x1, which is what the engine's
// Foundation_1x1 default means for the shipped rules (none of the six declares
// the key in rulesmd.ini - it is an art key). All six do declare it in
// artmd.ini: CAOILD 2x2, CAPOWR 2x2, CAMACH 3x3, CAAIRP 3x3, CAOUTP 4x3,
// CATHOSP/CAHOSP 6x4.
// ---------------------------------------------------------------------------
void RandomMapGenerator::LoadNeutralTechBuildings()
{
    neutralTechBuildings_.clear();

    char rulesPath[MAX_PATH];
    if (!FindGameIni(L"rulesmd.ini", rulesPath, MAX_PATH))
    {
        DiagLog("TECH-DIAG LoadNeutral: rulesmd.ini NOT FOUND by FindGameIni");
        return;
    }
    DiagLog("TECH-DIAG LoadNeutral: rules path='%s'", rulesPath);

    char list[512] = "";
    GetPrivateProfileStringA("AI", "NeutralTechBuildings", "",
                             list, sizeof(list), rulesPath);
    DiagLog("TECH-DIAG LoadNeutral: raw list='%s' len=%d",
            list, static_cast<int>(strlen(list)));
    if (list[0] == '\0')
        return;

    char artPath[MAX_PATH];
    const bool haveArt = FindGameIni(L"artmd.ini", artPath, MAX_PATH);
    DiagLog("TECH-DIAG LoadNeutral: artmd.ini found=%d path='%s'",
            haveArt ? 1 : 0, haveArt ? artPath : "(none)");

    char* context = nullptr;
    for (const char* name = strtok_s(list, ",", &context); name != nullptr;
         name = strtok_s(nullptr, ",", &context))
    {
        while (*name == ' ' || *name == '\t')
            ++name;

        NeutralTechBuilding entry;
        strncpy_s(entry.name, name, _TRUNCATE);

        if (haveArt)
        {
            char image[64] = "";
            GetPrivateProfileStringA(name, "Image", "", image,
                                     sizeof(image), artPath);

            char value[32] = "";
            if (image[0] != '\0')
                GetPrivateProfileStringA(image, "Foundation", "", value,
                                         sizeof(value), artPath);
            if (value[0] == '\0')
                GetPrivateProfileStringA(name, "Foundation", "", value,
                                         sizeof(value), artPath);

            int w = 0;
            int h = 0;
            if (value[0] != '\0' && sscanf_s(value, "%dx%d", &w, &h) == 2
                && w > 0 && h > 0)
            {
                entry.width = w;
                entry.height = h;
            }
        }

        neutralTechBuildings_.push_back(entry);
        DiagLog("TECH-DIAG LoadNeutral: entry[%d]='%s' %dx%d",
                static_cast<int>(neutralTechBuildings_.size()) - 1,
                entry.name, entry.width, entry.height);
    }

    DiagLog("TECH-DIAG LoadNeutral: total entries=%d",
            static_cast<int>(neutralTechBuildings_.size()));
}

// ---------------------------------------------------------------------------
// LoadMultiplayerHouses - the [Houses] ledger the multiplayer (.yrm) layout
// writes.
//
// FA2's multiplayer branch (CHouses::OnPreparehouses, Houses.cpp:253-284) takes
// the country list straight from the rules INI's [Countries] section and, for
// every country, registers the country name itself as a map house and stores
// that country's Color from the rules [<country>] section. The keys keep the
// [Countries] index order, so the .yrm carries the rules numbering
// (0=Americans ... 12=Neutral, 13=Special). A multiplayer map has no
// [Countries] section of its own - the shipped .yrm files match.
//
// The rules INI is located the same way LoadNeutralTechBuildings does it. When
// it is missing the list stays empty and SaveMapFile falls back to the neutral
// house alone.
// ---------------------------------------------------------------------------
void RandomMapGenerator::LoadMultiplayerHouses()
{
    multiplayerHouses_.clear();

    char rulesPath[MAX_PATH];
    if (!FindGameIni(L"rulesmd.ini", rulesPath, MAX_PATH))
        return;

    // The keys are contiguous from 0, so stop at the first absent index. The
    // sentinel default tells "no such key" apart from a present, empty value.
    for (int index = 0; index < 64; ++index)
    {
        char key[16];
        sprintf_s(key, sizeof(key), "%d", index);

        char country[64] = "";
        GetPrivateProfileStringA("Countries", key, "\x01", country,
                                 sizeof(country), rulesPath);
        if (country[0] == '\0' || country[0] == '\x01')
            break;

        MultiplayerHouse house;
        strncpy_s(house.country, country, _TRUNCATE);
        GetPrivateProfileStringA(country, "Color", "Grey", house.color,
                                 sizeof(house.color), rulesPath);

        multiplayerHouses_.push_back(house);
    }

    DiagLog("HOUSES-DIAG LoadMultiplayerHouses: countries=%d",
            static_cast<int>(multiplayerHouses_.size()));
}

// ---------------------------------------------------------------------------
// sub_595400 (0x595400 - 0x59567F) - the zero-share fallback.
//
// Full body in 595400.c (decompile folder). It runs for every selected region
// whose budget share came out 0, but only when the global n3 == 0 (see
// SelectStartingPointRegions) - n3 is 1..3 on the dialog path, so in practice
// this never runs; it is ported so the shape matches.
//
//   1. house = HouseClass::FindByCountryIndex(FindIndexOfName("Neutral"))
//      and one BuildingClass is created for a RANDOM entry of
//      RulesClass::NeutralTechBuildings - one draw, F2I64(Random() * count *
//      kUnitScale) with the usual "> count-1" rejection.
//
//   2. Up to 100 attempts. Each attempt draws one cell of the region (same
//      formula, one draw), takes that cell's Level, and walks the building
//      type's foundation: every foundation cell must be free of objects, carry
//      a placeholder tile, sit on the anchor's Level, not be Rock, be inside
//      the usable area and have a clear work byte +69. The first cell that
//      passes is where the building goes:
//          Unlimbo({ x * 256 + 128, y * 256 + 128, 0 }, DirType_North)
//      and the routine answers 1. Any failing cell ends the attempt.
//
//   3. All 100 attempts failed -> the building is deleted and 0 is answered.
//      (The caller ignores the answer.)
//
// Route B again: instead of the live BuildingClass the port appends a
// MapStructure and marks the foundation cells (RecordPlacedBuilding), which is
// the part of Unlimbo the next fit test observes (vanilla sees it through the
// cell's object chain).
//
// Deviation: when the rules INI (or its [General] list) is missing the port has
// no type to draw, so it answers 0 WITHOUT consuming the type draw. The shipped
// rulesmd.ini always has the list.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::GiveRegionATechBuilding(MapRegion* region)
{
    if (region == nullptr || workCells_ == nullptr)
        return false;
    if (region->cells.empty() || neutralTechBuildings_.empty())
        return false;

    // ---- 1. the type (0x595451 - 0x5954a6) --------------------------------
    const int typeCount = static_cast<int>(neutralTechBuildings_.size());
    int typeIndex;
    do
    {
        typeIndex = F2I64(static_cast<double>(rng_.Next())
                          * static_cast<double>(typeCount) * kUnitScale);
    }
    while (typeIndex > typeCount - 1);

    const NeutralTechBuilding& type = neutralTechBuildings_[
        static_cast<size_t>(typeIndex)];

    // ---- 2. up to 100 attempts (0x5954bb - 0x595647) ----------------------
    // [port-only] 先要求 1 格平坦围裙（避开刻区域阶段已存在的悬崖斜坡）；100
    // 次都找不到再用原版仅看地基的规则兜底，保证不比原版少放建筑。
    const int cellCount = static_cast<int>(region->cells.size());
    for (int pass = 0; pass < 2; ++pass)
    {
        const int apron = (pass == 0) ? 1 : 0;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            int index;
            do
            {
                index = F2I64(static_cast<double>(rng_.Next())
                              * static_cast<double>(cellCount) * kUnitScale);
            }
            while (index > cellCount - 1);

            const CellStruct cell = region->cells[static_cast<size_t>(index)];
            const int level = CellAt(cell.X, cell.Y)->Level;      // 0x595522

            if (!FoundationFitsAtFlat(cell, level,
                                      type.width, type.height, apron))
                continue;

            // The vanilla Unlimbos here (0x595608 - 0x595633) and answers 1.
            if (apron == 0)
                DiagLog("TECH-DIAG region site fell back to foundation-only rule (%s)",
                        type.name);
            RecordPlacedBuilding(cell, type.name, type.width, type.height);
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// The foundation fit test of sub_595400's inner loop (0x595548 - 0x5955e5).
//
// The vanilla walks the cell list BuildingTypeClass::GetFoundationData answers
// and rejects the position on the first bad cell:
//     pCell->FirstObject                  -> an object is in the way
//     !sub_486380(pCell)                  -> not a placeholder tile
//     pCell->Level != anchorLevel         -> a different layer
//     pCell->LandType == LandType_Rock
//     !IsWithinUsableArea_2(pCell, true)
//     work[cell + 69]                     -> the cliff / shore protection mark
//
// Two mappings to note:
//   - FirstObject: the port has no object chain, so this is
//     AltCellFlags_ContainsBuilding - the same stand-in
//     CreateNeutralBridgeRepairHut uses.
//   - the cell list: the port walks the type's W x H rectangle anchored at the
//     position cell - (col, row), col in [0, W), row in [0, H) - which is what
//     the engine's table holds (sub_45B1C0 fills it row-major from (0,0), and
//     FoundationData is 0x89C900 + Foundation * 120; see the NeutralTechBuilding
//     note in MapGen.h). GetFoundationData (0x45EC20) itself is a getter that
//     ignores includeBib and answers an empty list when the field is 0. The art
//     INI's AddOccupy / RemoveOccupy keys build the separate FoundationOutside
//     field, so they do not belong in this test.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::FoundationFitsAt(CellStruct base, int level,
                                          int width, int height)
{
    const int side = size_.workSide;

    for (int row = 0; row < height; ++row)
    {
        for (int col = 0; col < width; ++col)
        {
            const int16_t x = static_cast<int16_t>(base.X + col);
            const int16_t y = static_cast<int16_t>(base.Y + row);

            MapCell* cell = CellAt(x, y);
            if ((cell->AltFlags & AltCellFlags_ContainsBuilding) != 0)
                return false;
            if (!IsPlaceholderTile(cell))                 // 0x5955a6 sub_486380
                return false;
            if (cell->Level != level)                     // 0x5955b5
                return false;
            if (cell->LandType == 3)                      // 0x5955bf LandType_Rock
                return false;
            if (!IsWithinUsableArea(CellStruct{ x, y }, true))   // 0x5955c8
                return false;
            if (workCells_[x + side * y].Byte(69) != 0)   // 0x5955d6
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// FoundationFitsAtFlat [port-only]
//
// Vanilla fit on the foundation itself, plus every cell of an apron around it
// must be in the diamond, flat, land-walkable and at the same Level. Region
// ramps / cliff facades are slope tiles (SlopeIndex != 0) or non-walkable, so
// a building site wedged right against an existing ramp is rejected here.
// Already-tiled FLAT ground (the region body fills many cells with real clear
// textures, e.g. tiles 131/140) is accepted - only the tile kind matters, not
// whether it is still the placeholder. The usable-area / Rock / +69 / object
// clauses stay scoped to the foundation itself.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::FoundationFitsAtFlat(CellStruct base, int level,
                                               int width, int height, int apron)
{
    if (apron <= 0)
        return FoundationFitsAt(base, level, width, height);

    if (!FoundationFitsAt(base, level, width, height))
        return false;

    for (int row = base.Y - apron; row < base.Y + height + apron; ++row)
    {
        for (int col = base.X - apron; col < base.X + width + apron; ++col)
        {
            if (col >= base.X && col < base.X + width
                && row >= base.Y && row < base.Y + height)
                continue;                       // foundation: tested above

            const int16_t x = static_cast<int16_t>(col);
            const int16_t y = static_cast<int16_t>(row);
            if (!CellExists(x, y))
                return false;
            const MapCell* ring = CellAt(x, y);
            // Same elevation, no ramp tile, and land (plain or beach) only:
            // water / impassable cliff facades / outside cells keep the
            // building art away from slopes and shorelines.
            if (ring->Level != level || ring->SlopeIndex != 0)
                return false;
            if (ring->Passability != PassabilityType_Passable
                && ring->Passability != PassabilityType_Beach)
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Route B placement shared by sub_595400 and sub_5A95B0.
//
// Vanilla ends a successful placement with Unlimbo, which threads the new
// BuildingClass onto the object chain of EVERY foundation cell - that is what
// makes the FirstObject clause of FoundationFitsAt reject a later building that
// overlaps it. The port has no object chain, so each foundation cell of the
// W x H rectangle takes the same AltCellFlags_ContainsBuilding stand-in
// CreateNeutralBridgeRepairHut uses, and the building itself is appended to
// structures_ (the .map [Structures] source; see MapStructure).
// ---------------------------------------------------------------------------
void RandomMapGenerator::RecordPlacedBuilding(CellStruct base,
                                              const char* typeName,
                                              int width, int height)
{
    structures_.push_back(MapStructure(base, typeName, "Neutral House", 0 /*North*/));

    for (int row = 0; row < height; ++row)
    {
        for (int col = 0; col < width; ++col)
        {
            CellAt(static_cast<int16_t>(base.X + col),
                   static_cast<int16_t>(base.Y + row))->AltFlags
                |= AltCellFlags_ContainsBuilding;
        }
    }
}

// ---------------------------------------------------------------------------
// Step 2 - sub_5A1FB0 (0x5A1FB0 - 0x5A21AF): paint each starting point's
// surroundings.
//
// Full body in 598E9E_CreatingStartingPoints.c. For every starting point i
// (0-based, count = startingPointCount_):
//
//   1. coords = waypoints_[i]                           (GetWaypointCoords)
//   2. clear every diamond cell's work byte +15 (the start mark) to 0
//   3. seed: work[coords + 15] = i + 1, and push a distance-0 node for coords
//   4. flood: pop the nearest node. Once 400 cells have been painted the whole
//      point is done; otherwise stamp the popped cell's work byte +69 = 1 (the
//      "starting point" protection mark the hill stage avoids), then look at
//      its eight neighbours - an in-diamond neighbour whose +15 mark is 0 and
//      whose tile is a placeholder (sub_486380) goes into the pool with its
//      straight-line distance to the starting point, gets marked with i + 1 and
//      is pushed.
//   5. repeat until the heap drains; then the next starting point.
//
// No RNG. Always answers true (the stage's retry loop ends here).
//
// Two things about the vanilla's buffers:
//   - the pool is 800 nodes and a push is skipped once count + 1 reaches 800
//     (`if (v34 < v6[1])`), so a node discovered after that keeps its +15 mark
//     but is never queued. StartFloodHeap keeps that rule.
//   - the pool itself is modelled as a vector reserved to 3201 entries: the
//     flood paints at most 400 cells and every painted cell adds at most 8
//     nodes, so 1 + 400 * 8 is a hard bound and the heap's pointers stay valid.
//     The vanilla writes past its 800 nodes when a flood discovers that many
//     (undefined behaviour there, and not reproducible here).
// ---------------------------------------------------------------------------
bool RandomMapGenerator::PaintStartingPointTerrain()
{
    if (workCells_ == nullptr || cellSlots_ == nullptr)
        return true;

    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    const int side = size_.workSide;

    for (int i = 0; i < startingPointCount_; ++i)
    {
        if (i >= static_cast<int>(waypoints_.size()))
            break;                          // no waypoint stamped that far
        const CellStruct start = waypoints_[static_cast<size_t>(i)];

        std::vector<StartFloodNode> pool;
        pool.reserve(1 + 400 * 8);
        StartFloodHeap heap;

        // ---- clear every diamond cell's start mark ------------------------
        {
            CellIterator it;
            it.Reset(cellSlots_, size_.mapWidth);
            while (MapCell* cell = it.Next())
            {
                const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
                const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);
                workCells_[x + side * y].data[15] = 0;
            }
        }

        const int mark = i + 1;

        // ---- the seed ------------------------------------------------------
        pool.push_back(StartFloodNode{ start, 0.0f });
        workCells_[start.X + side * start.Y].data[15] = mark;
        heap.Push(&pool[0]);

        // ---- the flood -----------------------------------------------------
        int painted = 0;
        StartFloodNode* node = heap.Pop();
        while (node != nullptr)
        {
            if (painted >= 400)
                break;

            const CellStruct cur = node->coords;
            workCells_[cur.X + side * cur.Y].Byte(69) = 1;

            for (int dir = 0; dir < 8; ++dir)
            {
                const int16_t nx = static_cast<int16_t>(cur.X + kDirX[dir]);
                const int16_t ny = static_cast<int16_t>(cur.Y + kDirY[dir]);
                if (!CellExists(nx, ny))
                    continue;
                if (workCells_[nx + side * ny].data[15] != 0)
                    continue;

                MapCell* cell = CellAt(nx, ny);
                if (!IsPlaceholderTile(cell))               // sub_486380
                    continue;

                const int dx = nx - start.X;
                const int dy = ny - start.Y;

                StartFloodNode entry;
                entry.coords = CellStruct{ nx, ny };
                entry.distance = static_cast<float>(
                    std::sqrt(static_cast<double>(dx * dx + dy * dy)));

                pool.push_back(entry);
                workCells_[nx + side * ny].data[15] = mark;
                heap.Push(&pool.back());
            }

            ++painted;
            node = heap.Pop();
        }

        DiagLog("SP-PAINT pt%d start=(%d,%d) painted=%d",
                i, start.X, start.Y, painted);
    }

    // 诊断：对每一对出生点，检查二者之间能否沿"占位/可通行同高"格连通，
    // 以及路径中点是否被任一 byte69 保护区覆盖。
    if (startingPointCount_ >= 2)
    {
        static const int16_t pDX[4] = { 0, 1, 0, -1 };
        static const int16_t pDY[4] = { -1, 0, 1, 0 };
        for (int a = 0; a < startingPointCount_; ++a)
            for (int b = a + 1; b < startingPointCount_; ++b)
            {
                const CellStruct sa = waypoints_[static_cast<size_t>(a)];
                const CellStruct sb = waypoints_[static_cast<size_t>(b)];
                const int lvl = CellAt(sa.X, sa.Y)->Level;
                std::vector<int> q;
                std::vector<unsigned char> seen(static_cast<size_t>(side) * side, 0);
                q.push_back(sa.X + side * sa.Y);
                seen[sa.X + side * sa.Y] = 1;
                bool reachable = false;
                int pathLen = 0;
                std::vector<int> parent(static_cast<size_t>(side) * side, -1);
                size_t head = 0;
                while (head < q.size())
                {
                    const int idx = q[head++];
                    const int px = idx % side, py = idx / side;
                    if (px == sb.X && py == sb.Y) { reachable = true; break; }
                    for (int d = 0; d < 4; ++d)
                    {
                        const int nx = px + pDX[d], ny = py + pDY[d];
                        if (nx < 0 || ny < 0 || nx >= side || ny >= side)
                            continue;
                        const int ni = nx + side * ny;
                        if (seen[ni])
                            continue;
                        if (!CellExists(static_cast<int16_t>(nx), static_cast<int16_t>(ny)))
                            continue;
                        const MapCell* nc = CellAt(nx, ny);
                        if (nc->Passability != PassabilityType_Passable || nc->Level != lvl)
                            continue;
                        seen[ni] = 1;
                        parent[ni] = idx;
                        q.push_back(ni);
                    }
                }
                if (reachable)
                {
                    int cur = sb.X + side * sb.Y;
                    int unprotected = 0, total = 0;
                    while (cur >= 0)
                    {
                        ++total;
                        if (workCells_[cur].Byte(69) == 0)
                            ++unprotected;
                        if (cur == sa.X + side * sa.Y)
                            break;
                        cur = parent[cur];
                    }
                    pathLen = total;
                    DiagLog("SP-PATHTEST %d->%d lvl=%d reachable=YES path=%d unprotected69=%d",
                            a, b, lvl, pathLen, unprotected);
                }
                else
                {
                    DiagLog("SP-PATHTEST %d->%d lvl=%d reachable=NO (a=(%d,%d)L%d b=(%d,%d)L%d)",
                            a, b, lvl, sa.X, sa.Y,
                            CellAt(sa.X, sa.Y)->Level, sb.X, sb.Y,
                            CellAt(sb.X, sb.Y)->Level);
                }
            }
    }

    return true;
}
