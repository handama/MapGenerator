// ============================================================================
// MapGenMakingSub.cpp - the Making-regions (MapGenMaking.cpp) subordinates
//
// The stage body MakeRegions lives in MapGenMaking.cpp, which also carries the
// stage documentation and the list of the calls it makes; this file holds the
// subordinates themselves, one per vanilla routine, in the vanilla call order.
//
// Ported: every Making-regions routine. The one deviation is
// CreateNeutralBridgeRepairHut, which records the building instead of
// instantiating one (route B; see MapStructure in MapGen.h).
//
// Decompiles: 598D44_MakingRegions.c and the files it lists (58EBC0.c, 58EF10.c,
// 5A19E0.c, 578E60.c, 5A17F0.c, 59B740.c) in the project decompile folder.
// ============================================================================

#include "pch.h"
#include "MapGen.h"
#include "MapGenAtanTable.h"
#include <cmath>

// ---------------------------------------------------------------------------
// The region size gate (0x58ed89 - 0x58edae) multiplies three values that all
// live on the RMG instance, so they are read live instead of as constants:
//
//   dword_ABE048 = this[28] = the rolled RegionSize option   (globalOptions_)
//   dword_ABE15C = this[97] = the map HEIGHT                 (size_.height)
//   dword_ABE158 = this[96] = the map WIDTH                  (size_.width)
//
// An earlier reading of this file held all three to be "never written, image
// value 0" because xrefs_to lists no writer. That inference is INVALID for
// instance fields: sub_597260 rolls RegionSize with a register-relative store
// (0x5972f6 "mov [edi+70h], eax") and sub_599650 computes Width/Height
// (0x599700 "this[96] = F2I64(w)", 0x599748 "this[97] = ..."), neither of which
// leaves an xref behind. The names come from the RMG preset INI - sub_597A30
// reads [RandomMap] Key=Width / Height / RegionSize straight into
// this[25]/this[26]/this[28] (0x597a24, 0x597b3d, 0x597bd9).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// sub_4CADE0 - the vanilla's table-based atan (used by sub_58C6F0 and the
// river/lake shore priority keys). The exact 4097 float32 entries live in
// MapGenAtanTable.h (dumped from flt_8610B4); they differ by 1-14 ULP from
// std::atan round-to-nearest doubles, and sub_58D620's growth heap walks
// different cells on those differences. The divisor is the float flt_8650B8
// promoted to double for the x87 divide, the index truncates toward zero, and
// abs(index) >= 4097 saturates to 1.5707964f (0x3FC90FDB).
double TableAtan(double x)
{
    const int index = F2I64(std::fabs(x)
                            / static_cast<double>(MapGenAtanTable::kStep));
    double v;
    if (index >= 4097)
        v = static_cast<double>(MapGenAtanTable::kSaturation);
    else
        v = static_cast<double>(MapGenAtanTable::kTable[index]);
    return (x < 0.0) ? -v : v;
}

namespace {

const double kTwoPi      = 6.283185307179586;      // dbl_7E3CC0
const double kPi         = 3.141592653589793;      // dbl_7E44D0 / dbl_7E2820
const double kPiOverEight = 0.3926990816987241;    // dbl_7ED8A0
// Random() * 2 pi / 2^32 - the growth loop's initial heading (0x58d... fild /
// fmul 1.462918079607772e-9, i.e. 2 pi / 2^32; NOT kUnitScale, which carries the
// extra (1 + 2^-32) factor).
const double kAngleScale = 1.462918079607772e-9;

// One growth node: the packed cell coords plus the float priority key (the
// vanilla's 8-byte node entries, `operator new(8 * (2 * cellCount + 10))` at
// 0x58d640; the key sits at +4).
struct RegionNode
{
    CellStruct coords;      // +0
    float      key;         // +4
};

// NodeHeap::Pop's "nothing left" answer.
const size_t kNoNode = static_cast<size_t>(-1);

// The vanilla's 0x14-byte priority queue { count, capacity, items, maxSeen,
// minSeen } - a min-heap on the node key whose item array lives in the node
// array. SiftDown is sub_5AD870, the very same routine the lake stage's heap
// (MapGenLake.cpp, LakeHeap) ports. maxSeen / minSeen are dead stores and are
// dropped; slots hold node INDICES so the node vector may reallocate safely.
class NodeHeap
{
public:
    bool Empty() const { return slots_.empty(); }

    // 0x58da63 - 0x58dabf: bubble the new node up while its key is smaller.
    void Push(const std::vector<RegionNode>& nodes, size_t index)
    {
        slots_.push_back(index);
        size_t i = slots_.size();                      // 1-based during the walk
        while (i > 1)
        {
            const size_t parent = i >> 1;
            if (nodes[slots_[parent - 1]].key <= nodes[index].key)
                break;
            slots_[i - 1] = slots_[parent - 1];
            i = parent;
        }
        slots_[i - 1] = index;
    }

    // 0x58db09 - 0x58db2a: take the root, move the last slot into it, sift down.
    size_t Pop(const std::vector<RegionNode>& nodes)
    {
        if (slots_.empty())
            return kNoNode;
        const size_t top = slots_[0];
        slots_[0] = slots_.back();
        slots_.pop_back();
        if (!slots_.empty())
            SiftDown(nodes, 0);
        return top;
    }

private:
    // sub_5AD870 - the smaller-key child bubbles up until both children are
    // larger (or absent).
    void SiftDown(const std::vector<RegionNode>& nodes, size_t i)
    {
        const size_t count = slots_.size();
        const size_t moved = slots_[i];
        do
        {
            const size_t child = 2 * i + 1;
            if (child >= count)
                break;
            size_t pick = child;
            if (child + 1 < count
                && nodes[slots_[child + 1]].key < nodes[slots_[child]].key)
                pick = child + 1;
            if (nodes[slots_[pick]].key >= nodes[moved].key)
                break;
            slots_[i] = slots_[pick];
            i = pick;
        }
        while (true);
        slots_[i] = moved;
    }

    std::vector<size_t> slots_;
};

// Packed CellStruct <-> int (the vanilla's `Cell` is the packed dword).
int PackCoords(CellStruct c)
{
    return (static_cast<int>(static_cast<uint16_t>(c.X)) & 0xFFFF)
         | (static_cast<int>(static_cast<uint16_t>(c.Y)) << 16);
}

// The 8 neighbour offsets in Neighbours order (0x89F688):
//   N (0,-1)  NE (1,-1)  E (1,0)  SE (1,1)  S (0,1)  SW (-1,1)  W (-1,0)  NW (-1,-1)
const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

// The global n0x64 @0xABE044 that sub_5905D0 gates the ramp count on is the RMG
// instance's this[27] (base 0xABDFD8 + 0x6C = 0xABE044): sub_597260 rolls it as
// RandomRanged(0, 0x82B0D0[n3]) - 0x5972D9 "mov [edi+6Ch], eax" - and sub_5905D0
// reads it back absolutely at 0x5907B8 ("cmp eax, n0x64"). The comparison is
//     do { roll = F2I64(Random() * 2.3515895e-8); } while (roll > 100);
//     if (roll >= ::n0x64) ramps = 0; else draw 1..2;
// so the gate is the map's ROLLED 0..100 value, not the 0 the image happens to
// hold there (that is only the pre-load filler; sub_597260 always overwrites it
// before the generation runs). Reading it live matters twice over: it picks the
// ramp count AND, when the branch is taken, it makes the 1..2 draw consume the
// map RNG (dword_ABE890) - so a wrong gate desynchronises every later draw.
// The value lives on the instance in globalOptions_.accessibility.

// The [Map] LocalSize offsets - VisibleRect.X and VisibleRect.Y - that
// IsWithinUsableArea uses.
const int kVisibleOffsetX = 2;
const int kVisibleOffsetY = 5;

}   // namespace

// sub_58EBC0 - Making-regions step 1: rebuild every region body, then settle the
// small ones and merge the big ones away.
//
// Vanilla body (58EBC0.c, disasm 0x58ebd0 - 0x58ee90):
//   1. per region (0x58ebd0 - 0x58ec11): clear the embedded
//      DynamicVectorClass<Cell> through its vtable (+0x0C), set cellCount
//      (+0x0C) to 0 and the bounding rect (+0x40) to { 9999, 9999, 0, 0 } - the
//      height is zeroed by the for-loop's increment clause, which runs after
//      every body including the last one;
//   2. rescan the work square BACKWARDS (0x58ec28 - 0x58ed83, index
//      side*side-1 down to 0): every in-diamond work cell whose data[14] is a
//      valid region index (0 <= mark < count) is counted into that region
//      (0x58ecb0), appended to its cell list (0x58ecf8, Count at +0x38 /
//      Items at +0x2C) and folded into its bounding rect (0x58ecff - 0x58ed6a);
//   3. threshold = F2I64(2 * ((0.005 * dword_ABE048 + 0.05) * dword_ABE15C *
//      dword_ABE158)) (0x58ed89 - 0x58edae; see the constants at the top);
//   4. settle / merge pass (0x58edb5 - 0x58ee8b): for every region with byte +26
//      == 0 and byte +20 == 0 (not water family), a cellCount within the
//      threshold settles it (byte +26 = 1, 0x58eded); otherwise sub_58D620 gets
//      the region and, when it reports the region consumed, the region is
//      removed and deleted - the vanilla inlines sub_58C070's body here (vtable
//      guard 0x58ee05, array compaction 0x58ee29 - 0x58ee5c, vector teardown
//      0x58ee5e, operator delete 0x58ee83), which is exactly what our
//      ReleaseRegionObject(region, true) does - and the whole pass restarts at
//      index 0 (0x58ee8b `goto LABEL_32`) because the array indices shifted.
//
// The marks are used as ARRAY INDICES here, not as ids: BuildRegionFromCell
// hands ids out in append order, so id == index as long as nothing was removed.
//
// RNG: this function draws nothing itself, but step 4's sub_58D620 does (four
// dst_ @0xABE890 references) - while that callee is still a stub, those draws do
// not happen, so the RNG stream diverges from vanilla from here until sub_58D620
// lands.
// ---------------------------------------------------------------------------
void RandomMapGenerator::RebuildRegionBodies()
{
    if (workCells_ == nullptr)
        return;

    const int side = size_.workSide;

    // ---- 1. Reset every region (0x58ebd0 - 0x58ec11) ----------------------
    for (size_t i = 0; i < regions_.size(); ++i)         // 0x58ebd0
    {
        MapRegion* region = regions_[i];
        if (region == nullptr)
            continue;
        region->cells.clear();                           // 0x58ebe9 (vtable +0x0C)
        region->cellCount = 0;                           // 0x58ebf4
        region->bounds.X      = 9999;                    // 0x58ec0c
        region->bounds.Y      = 9999;                    // 0x58ec0e
        region->bounds.Width  = 0;                       // 0x58ec11
        region->bounds.Height = 0;                       // increment clause
    }

    // ---- 2. Regroup the work square, backwards (0x58ec28 - 0x58ed83) ------
    for (int idx = side * side - 1; idx >= 0; --idx)     // 0x58ec28 / 0x58ed73
    {
        WorkCell& work = workCells_[idx];
        const int packed = work.MapCoords();             // 0x58ec49
        const int16_t x = static_cast<int16_t>(packed & 0xFFFF);
        const int16_t y = static_cast<int16_t>((uint32_t)packed >> 16);
        if (!CellExists(x, y))                           // 0x58ec81
            continue;

        const int mark = work.data[14];                  // 0x58ec8c
        if (mark < 0 || mark >= static_cast<int>(regions_.size()))
            continue;                                    // 0x58ec9e
        MapRegion* region = regions_[mark];
        if (region == nullptr)
            continue;

        ++region->cellCount;                             // 0x58ecb0
        region->cells.push_back(CellStruct{ x, y });     // 0x58ecf8

        MapRectTag& r = region->bounds;                  // 0x58ecff
        if (r.Width == 0)                                // 0x58ed0a
        {
            r.X      = x;                                // 0x58ed16
            r.Y      = y;                                // 0x58ed18
            r.Width  = 1;                                // 0x58ed1b
            r.Height = 1;                                // 0x58ed1e
        }
        const int minX = r.X;                            // 0x58ed21
        if (x >= r.X + r.Width)                          // 0x58ed2c
            r.Width = x - minX + 1;                      // 0x58ed33
        if (x < minX)                                    // 0x58ed38
        {
            const int w = r.Width;                       // 0x58ed3a
            r.X = x;                                     // 0x58ed41
            r.Width = minX - x + w;                      // 0x58ed43
        }
        const int minY = r.Y;                            // 0x58ed46
        if (y >= r.Y + r.Height)                         // 0x58ed52
            r.Height = y - minY + 1;                     // 0x58ed59
        if (y < minY)                                    // 0x58ed5e
        {
            const int h = r.Height;                      // 0x58ed60
            r.Y = y;                                     // 0x58ed67
            r.Height = minY - y + h;                     // 0x58ed6a
        }
    }

    // ---- 3. Region size gate (0x58ed89 - 0x58edae) ------------------------
    const int threshold = F2I64(
        2.0 * ((0.005 * static_cast<double>(globalOptions_.regionSize) + 0.05)
               * static_cast<double>(size_.width)
               * static_cast<double>(size_.height)));

    // ---- 4. Settle / merge pass (0x58edb5 - 0x58ee8b) ---------------------
    size_t j = 0;                                        // 0x58edbe (LABEL_32)
    while (j < regions_.size())
    {
        MapRegion* region = regions_[j];                 // 0x58edca
        if (region == nullptr
            || region->mergeSettled != 0                 // 0x58edd4
            || region->waterFamily)
        {
            ++j;
            continue;
        }

        if (region->cellCount <= threshold)              // 0x58edde
        {
            region->mergeSettled = 1;                    // 0x58eded
            ++j;
            continue;
        }

        if (SplitOrDropRegion(region))                   // 0x58ede2
        {
            ReleaseRegionObject(region, true);           // 0x58ee03 - 0x58ee83
            // Vanilla's `goto LABEL_32` (0x58ee8b) lands on the for-loop's own
            // increment, so the scan simply moves on - it does NOT restart from
            // the head. ReleaseRegionObject compacts the array, so this `++j`
            // also steps over the record that slid down into slot j. Restarting
            // from 0 (an earlier reading of this loop) made the pass unable to
            // ever reach the end: each split removes one record but its new
            // sub-records are appended at the tail, so j never catches up.
            ++j;
            continue;
        }
        ++j;
    }
}

// sub_58C6F0 - the priority key of one growth node (0x58c6f0 - 0x58c7ff).
//
//   dx = cell.X - seed.X, dy = cell.Y - seed.Y, dist = sqrt(dx*dx + dy*dy)
//   theta = (dx == 0) ? pi/2 : TableAtan(-(dy/dx)) + (dx < 0 ? pi : 0)
//   diff  = |theta - angle|, wrapped into [0, 2 pi) and folded to [0, pi]
//   key   = Random() * kUnitScale * 2.5 + diff * 1.5 + dist * 0.15
//
// (0x58c6fc - 0x58c71b the deltas; 0x58c73e the dx == 0 short cut to pi/2
// dbl_7E2820; 0x58c754 sub_4CADE0; 0x58c760 the + pi for dx < 0; 0x58c76b the
// fabs and 0x58c77e the 2 pi wrapping; 0x58c795 the fold at pi; 0x58c7b5
// Randomizer::Random, then fmul kUnitScale, fmul 2.5, fmul 1.5 on diff, fmul
// 0.15 on dist.) One RNG draw per call.
double RandomMapGenerator::RegionNodePriority(CellStruct seed, CellStruct cell,
                                              double angle)
{
    const int dx = cell.X - seed.X;
    const int dy = cell.Y - seed.Y;
    const double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));

    double theta;
    if (dx != 0)
    {
        theta = TableAtan(-(static_cast<double>(dy) / static_cast<double>(dx)));
        if (dx < 0)
            theta += kPi;
    }
    else
    {
        theta = kPi / 2.0;
    }

    double diff = std::fabs(theta - angle);
    while (diff >= kTwoPi)
        diff -= kTwoPi;
    if (diff > kPi)
        diff = kTwoPi - diff;

    return static_cast<double>(static_cast<uint32_t>(rng_.Next())) * kUnitScale * 2.5
         + diff * 1.5
         + dist * 0.15;
}

// sub_58D620 - the merge pass's per-region callback: split / drop one large
// region (called from RebuildRegionBodies step 4 when cellCount > threshold).
//
// Vanilla body (58D620.c, disasm 0x58d62c - 0x58e5cb). It always reports the
// region as consumed (0x58e5c3 `mov al, 1`) unless byte +26 was already set, and
// the caller then destroys the record it handed in; the cells live on inside the
// records this routine creates:
//
//   1. 0x58d63b - 0x58d734: allocate the node buffer (8 * (2*cellCount + 10)
//      bytes) and the 0x14-byte heap object; clear the heap.
//   2. 0x58d73f - 0x58d7f0: work[+56] = -1 over the whole square, then
//      work[+56] = -2 on every cell of this region's list.
//   3. 0x58d63b / 0x58d64c / 0x58d6xx: target = cellCount/8 +
//      F2I64(Random * (cellCount/3 - cellCount/8 + 1) * kUnitScale)   [RNG]
//      and seed = cells[F2I64(Random * count * kUnitScale)]            [RNG];
//      the seed node (key 0) is pushed and work[seed].data[15] = -3.
//   4. 0x58d8xx - 0x58db3a: priority growth. heading = Random * 2 pi / 2^32
//      [RNG]. Pop the best node; mark its cell work[+56] = -3; for the FOUR
//      orthogonal neighbours inside the diamond whose work[+56] is still -2,
//      whose work[+60] != -3 and which are placeholder tiles: push a node with
//      key = sub_58C6F0(seed, n, heading) [RNG, one per accepted neighbour] and
//      set work[n].data[15] = -3. Then ++grown, jitter the heading by
//      Gaussian() * pi/8 [RNG] and pop the next node. Stops when grown reaches
//      the target or the heap runs dry.
//   5. 0x58db3b - 0x58dc1c: work[+60] = -1 everywhere; drain the remaining heap
//      nodes setting their cells' work[+56] = -3 (so every accepted neighbour is
//      carved, even the ones never popped); free the buffer and the heap.
//   6. 0x58dc5d - 0x58e4f9: walk this region's cell list BACKWARDS; every cell
//      whose work[+56] is -2 or -3 starts a new record through sub_58BF70
//      (CreateRegionRecord - it keeps the anchor cell's Level and family), the
//      cell is appended to it, the record joins the new-regions list, and a
//      flood (8 directions, LIFO) pulls in every contiguous cell with the same
//      mark, appending it and re-marking it with the new record's id. The carved
//      (-3) and the leftover (-2) cells therefore end up in two records.
//   7. 0x58df8x - 0x58e52x: per new record (from the end), skip it when byte
//      +0x24 ("consumed") is set. Otherwise collect its boundary cells
//      (sub_58E5D0 - the same "region cell with a differing in-diamond
//      neighbour" set as sub_58D410, here walked over the record's own list),
//      mark every region touching it in a byte map indexed by region id, and
//      from that set take the biggest non-water neighbour plus the min / max
//      Level. Then build the candidate Level list - span 8 -> {(max+min)/2};
//      span 4 -> {max} + the two guarded entries; span 0 -> the two guarded
//      entries - where an entry is minLevel-4 when minLevel >= 4 and maxLevel+4
//      when maxLevel <= 7; for LandType 3/4 (global n3 @0xABE014) the entries
//      equal to 0 are dropped. Finally:
//        - cellCount <= 100 and a neighbour exists: adopt the neighbour's Level,
//          shift this record's cells (the diamond walk comparing sub_58D0A0's
//          work mark with the record id) by the difference, hand every cell over
//          to the neighbour (+= cellCount, work mark = neighbour id) and set byte
//          +0x24 (0x58e4eb);
//        - otherwise: pick a candidate at random [RNG], set the record's Level to
//          it and shift its cells by the difference.
//   8. 0x58e52x - 0x58e55x: destroy every record whose byte +0x24 is set, except
//      the one passed in. 0x58e561 then bumps the passed-in record's +0x1C split
//      counter (not modelled - nothing reads it).
//
// RNG draws: the target, the seed index, the initial heading, one per accepted
// neighbour, the per-step gaussian, and (big regions only) the candidate pick.
// The port reproduces both the count and the order.
bool RandomMapGenerator::SplitOrDropRegion(MapRegion* region)
{
    if (region == nullptr || workCells_ == nullptr)
        return false;
    if (region->mergeSettled != 0)                        // 0x58d631
        return false;

    DiagLog("SPLIT-IN id=%d cells=%d level=%d water=%d",
            region->id, region->cellCount, region->level,
            region->waterFamily ? 1 : 0);

    const int side = size_.workSide;
    const int workCount = side * side;

    // ---- 1. Node buffer + heap (0x58d63b - 0x58d734) ----------------------
    std::vector<RegionNode> nodes;                        // vanilla's Block
    nodes.reserve(static_cast<size_t>(2 * region->cellCount + 10));
    NodeHeap heap;

    // ---- 2. Mark the square: -1 everywhere, -2 on this region's cells -----
    for (int idx = 0; idx < workCount; ++idx)             // 0x58d73f
        workCells_[idx].data[14] = -1;
    for (size_t i = region->cells.size(); i-- > 0;)       // 0x58d7xx (from the end)
    {
        const CellStruct c = region->cells[i];
        workCells_[c.X + side * c.Y].data[14] = -2;
    }

    // ---- 3. Target size and seed cell (both one RNG draw) -----------------
    const int hi = region->cellCount / 3;                 // 0x58d64c
    const int lo = region->cellCount / 8;
    int target;
    do
    {
        target = lo + F2I64(                            // 0x58d6xx (rejection is dead:
            static_cast<double>(static_cast<uint32_t>(rng_.Next()))
            * static_cast<double>(hi - lo + 1) * kUnitScale);
    }                                                   // the scaled value is < hi)
    while (target > hi);

    const int cellTotal = static_cast<int>(region->cells.size());
    int seedIndex;
    do
    {
        seedIndex = F2I64(                              // 0x58d6xx
            static_cast<double>(static_cast<uint32_t>(rng_.Next()))
            * static_cast<double>(cellTotal) * kUnitScale);
    }
    while (seedIndex > cellTotal - 1);

    const CellStruct seed = region->cells[static_cast<size_t>(seedIndex)];
    DiagLog("SPLIT-GROW id=%d target=%d lo=%d hi=%d seedIdx=%d seed=(%d,%d)",
            region->id, target, lo, hi, seedIndex, seed.X, seed.Y);
    workCells_[seed.X + side * seed.Y].data[15] = -3;                   // 0x58d6xx
    {
        RegionNode root;
        root.coords = seed;
        root.key = 0.0f;                                                // 0x58d6xx
        nodes.push_back(root);
        heap.Push(nodes, 0);
    }

    // ---- 4. Priority growth (0x58d8xx - 0x58db3a) -------------------------
    static const int16_t kGrowDirX[4] = { 0, 1, 0, -1 };   // Neighbours 0 / 2 / 4 / 6
    static const int16_t kGrowDirY[4] = { -1, 0, 1, 0 };

    double heading = static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                   * kAngleScale;                                       // 0x58d8xx [RNG]
    DiagLog("SPLIT-HEADING id=%d heading=%.4f", region->id, heading);
    int grown = 0;
    const char* growStop = "heap-dry";
    size_t cur = heap.Pop(nodes);                          // the seed node, key 0
    while (cur != kNoNode)
    {
        if (grown >= target)                               // 0x58d8xx
        {
            growStop = "target-reached";
            break;
        }

        const CellStruct cc = nodes[cur].coords;
        workCells_[cc.X + side * cc.Y].data[14] = -3;       // 0x58d9xx (carved)

        for (int d = 0; d < 4; ++d)
        {
            const int16_t nx = static_cast<int16_t>(cc.X + kGrowDirX[d]);
            const int16_t ny = static_cast<int16_t>(cc.Y + kGrowDirY[d]);
            if (!CellExists(nx, ny))                       // 0x58d991 - 0x58d9b7
                continue;

            WorkCell& w = workCells_[nx + side * ny];
            if (w.data[14] != -2)                          // 0x58d9dd
                continue;
            if (w.data[15] == -3)                          // 0x58d9ea
                continue;
            if (!IsPlaceholderTile(CellAt(nx, ny)))        // 0x58da05
                continue;

            RegionNode node;
            node.coords = CellStruct{ nx, ny };
            node.key = static_cast<float>(               // 0x58da31 [RNG]
                RegionNodePriority(seed, node.coords, heading));
            nodes.push_back(node);                        // 0x58da1e
            w.data[15] = -3;                              // 0x58da5b
            heap.Push(nodes, nodes.size() - 1);
        }

        ++grown;                                          // 0x58dae2
        heading += rng_.Gaussian() * kPiOverEight;        // 0x58daf0 [RNG]
        cur = heap.Pop(nodes);                            // 0x58db09
    }

    // ---- 5. Reset the visited marks, drain the heap (0x58db3b - 0x58dc1c) --
    for (int idx = 0; idx < workCount; ++idx)             // 0x58db3b
        workCells_[idx].data[15] = -1;
    while (true)                                          // 0x58db5d
    {
        const size_t n = heap.Pop(nodes);
        if (n == kNoNode)
            break;
        const CellStruct c = nodes[n].coords;
        workCells_[c.X + side * c.Y].data[14] = -3;        // 0x58dbea
    }
    {
        int nCarved = 0, nLeft = 0;
        for (size_t i = region->cells.size(); i-- > 0;)
        {
            const CellStruct c = region->cells[i];
            const int m = workCells_[c.X + side * c.Y].data[14];
            if (m == -3) ++nCarved;
            else if (m == -2) ++nLeft;
        }
        DiagLog("SPLIT-GROWN id=%d popped=%d stop=%s carved=%d leftover=%d nodes=%zu",
                region->id, grown, growStop, nCarved, nLeft, nodes.size());
    }

    // ---- 6. Re-assign the marked cells to new records (0x58dc5d - 0x58e4f9)
    std::vector<MapRegion*> created;                      // vanilla's v167 / v170
    std::vector<CellStruct> pending;

    for (int k = static_cast<int>(region->cells.size()) - 1; k >= 0; --k)
    {
        const CellStruct cell = region->cells[static_cast<size_t>(k)];
        const int mark = workCells_[cell.X + side * cell.Y].data[14];
        if (mark >= -1)                                   // only -2 / -3 were taken
            continue;

        MapRegion* nr = CreateRegionRecord(PackCoords(cell));   // 0x58dcc8
        nr->cells.push_back(cell);                        // 0x58dcex
        ++nr->cellCount;
        created.push_back(nr);                            // 0x58dxxx
        workCells_[cell.X + side * cell.Y].data[14] = nr->id;   // 0x58e0xx

        pending.clear();                                  // 0x58ddxx
        pending.push_back(cell);
        while (!pending.empty())
        {
            const CellStruct c = pending.back();
            pending.pop_back();
            for (int dir = 0; dir < 8; ++dir)
            {
                const int16_t nx = static_cast<int16_t>(c.X + kDirX[dir]);
                const int16_t ny = static_cast<int16_t>(c.Y + kDirY[dir]);
                if (!CellExists(nx, ny))                  // 0x58e2xx
                    continue;
                WorkCell& w = workCells_[nx + side * ny];
                if (w.data[14] != mark)                   // 0x58e2xx
                    continue;
                nr->cells.push_back(CellStruct{ nx, ny });
                ++nr->cellCount;
                w.data[14] = nr->id;
                pending.push_back(CellStruct{ nx, ny });
            }
        }

        {
            int minX = cell.X, maxX = cell.X, minY = cell.Y, maxY = cell.Y;
            for (size_t ci = 0; ci < nr->cells.size(); ++ci)
            {
                const CellStruct& qc = nr->cells[ci];
                if (qc.X < minX) minX = qc.X;
                if (qc.X > maxX) maxX = qc.X;
                if (qc.Y < minY) minY = qc.Y;
                if (qc.Y > maxY) maxY = qc.Y;
            }
            DiagLog("SPLIT-REC id=%d from=%s anchor=(%d,%d) level=%d cells=%d bbox=(%d,%d)-(%d,%d)",
                    nr->id, (mark == -3) ? "carved" : "leftover",
                    cell.X, cell.Y, nr->level, nr->cellCount,
                    minX, minY, maxX, maxY);
        }
    }

    // ---- 7. Neighbour analysis + Level settle (0x58df8x - 0x58e52x) -------
    for (int i = static_cast<int>(created.size()) - 1; i >= 0; --i)
    {
        MapRegion* nr = created[static_cast<size_t>(i)];
        if (nr->consumed != 0)                            // 0x58dfaf
            continue;

        // The boundary cells: sub_58E5D0 collects the record's own cells that
        // have an in-diamond neighbour with a different mark - the same set
        // sub_58D410 yields, only swept in the record's list order. This step
        // only needs the SET (it feeds the touching-regions map below), so the
        // shared collector is used.
        const std::vector<CellStruct> boundary = CollectRegionBoundary(nr);  // 0x58dfbc

        std::vector<unsigned char> touching(           // 0x58dfc1 (indexed by id)
            static_cast<size_t>(regionIdCounter_), 0);
        for (size_t b = boundary.size(); b-- > 0;)
        {
            const CellStruct c = boundary[b];
            for (int dir = 0; dir < 8; ++dir)
            {
                const int16_t nx = static_cast<int16_t>(c.X + kDirX[dir]);
                const int16_t ny = static_cast<int16_t>(c.Y + kDirY[dir]);
                if (!CellExists(nx, ny))                  // 0x58e0xx
                    continue;
                const int m = workCells_[nx + side * ny].data[14];
                if (m >= 0 && m != nr->id)                 // 0x58e101
                    touching[static_cast<size_t>(m)] = 1;
            }
        }

        // The biggest touching non-water record, plus the Level range.
        MapRegion* best = nullptr;                        // v147
        int  bestSize = -1;                               // v157
        int  bestLevel = -1;                              // v160
        int  minLevel = -1, maxLevel = -1;                // n4 / n7
        for (int id = 0; id < regionIdCounter_; ++id)     // 0x58e0da
        {
            if (touching[static_cast<size_t>(id)] == 0)
                continue;

            MapRegion* r2 = FindRegionById(id);           // the id -> record lookup
            if (r2 == nullptr)                            // vanilla would read past
                continue;                                 // the array here
            if (r2->consumed != 0)                        // 0x58e... 
                continue;

            if (r2->cellCount > bestSize && !r2->waterFamily)   // 0x58e0xx
            {
                bestSize = r2->cellCount;
                best = r2;
                bestLevel = r2->level;
            }
            if (minLevel == -1)                           // 0x58e140
            {
                minLevel = r2->level;
                maxLevel = minLevel;
            }
            else
            {
                if (r2->level > maxLevel)
                    maxLevel = r2->level;
                if (r2->level < minLevel)
                    minLevel = r2->level;
            }
        }

        // Candidate Levels for this piece (0x58e16a - 0x58e341).
        std::vector<int> candidates;
        const int span = maxLevel - minLevel;
        if (span == 8)                                    // 0x58e191
        {
            candidates.push_back((maxLevel + minLevel) / 2);     // 0x58e19a
        }
        else
        {
            if (span == 4)                                // 0x58e18c
                candidates.push_back(maxLevel);           // 0x58e222
            if (minLevel >= 4)                            // 0x58e... 
                candidates.push_back(minLevel - 4);
            if (maxLevel <= 7)                            // 0x58e... 
                candidates.push_back(maxLevel + 4);
        }
        if (static_cast<int>(config_.landType) == 3        // n3 @0xABE014 == 3 / 4
            || static_cast<int>(config_.landType) == 4)
        {
            size_t w = 0;                                 // 0x58e308 - 0x58e341
            for (size_t r = 0; r < candidates.size(); ++r)
            {
                if (candidates[r] != 0)
                    candidates[w++] = candidates[r];
            }
            candidates.resize(w);
        }

        if (nr->cellCount <= 100)                          // 0x58e354
        {
            {
                std::string cs;
                for (size_t ci = 0; ci < candidates.size(); ++ci)
                    cs += (ci ? "," : "") + std::to_string(candidates[ci]);
                DiagLog("SPLIT-SETTLE id=%d cells=%d minL=%d maxL=%d span=%d best=%s cands=[%s]",
                        nr->id, nr->cellCount, minLevel, maxLevel, span,
                        best ? ("id" + std::to_string(best->id)
                               + "/L" + std::to_string(bestLevel)
                               + "/n" + std::to_string(best->cellCount)).c_str()
                             : "NONE",
                        cs.c_str());
            }
            if (best == nullptr)                           // 0x58e40a: nothing to do
                continue;

            // Hand the piece over to the biggest neighbour: adopt its Level,
            // shift the piece's cells by the difference, move them across.
            const int oldLevel = nr->level;                // v129
            nr->level = bestLevel;                         // 0x58e422
            const int delta = bestLevel - oldLevel;        // v130 (a byte in vanilla)
            DiagLog("SPLIT-MERGE id=%d cells=%d oldL=%d -> best=%d L=%d delta=%d",
                    nr->id, nr->cellCount, oldLevel, best->id, bestLevel, delta);

            CellIterator it;                               // 0x58e427
            it.Reset(cellSlots_, size_.mapWidth, slotStride_);
            while (MapCell* c = it.Next())                 // 0x58e3cc
            {
                const int16_t x = static_cast<int16_t>(c->MapCoords & 0xFFFF);
                const int16_t y = static_cast<int16_t>((uint32_t)c->MapCoords >> 16);
                if (workCells_[x + side * y].data[14] == nr->id)   // 0x58e3e5
                {
                    const int before = c->Level;
                    c->Level += delta;                     // 0x58e3ef
                    if (delta != 0)
                        DiagLog("SPLIT-SHIFT (%d,%d) L%d->%d id=%d merge->%d",
                                x, y, before, c->Level, nr->id, best->id);
                }
            }

            for (size_t c = nr->cells.size(); c-- > 0;)    // 0x58e410 - 0x58e4f5
            {
                const CellStruct cc = nr->cells[c];
                best->cells.push_back(cc);                 // 0x58e4xx
                ++best->cellCount;
                workCells_[cc.X + side * cc.Y].data[14] = best->id;
            }
            nr->consumed = 1;                              // 0x58e4eb
        }
        else if (!candidates.empty())
        {
            // Big piece: pick one of the candidate Levels at random (0x58e343 -
            // 0x58e39d) and shift the piece's cells by the difference.
            // NOTE: vanilla has no empty-list guard here - with an empty list the
            // rejection loop below would spin forever. The list can only be empty
            // for the span-8 case with (max+min)/2 == 0, which this branch skips.
            int pick;
            do
            {
                pick = F2I64(                              // [RNG]
                    static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                    * static_cast<double>(candidates.size()) * kUnitScale);
            }
            while (pick > static_cast<int>(candidates.size()) - 1);

            const int chosen = candidates[static_cast<size_t>(pick)];
            const int delta = chosen - nr->level;          // v125
            nr->level = chosen;                            // 0x58e3xx
            {
                std::string cs;
                for (size_t ci = 0; ci < candidates.size(); ++ci)
                    cs += (ci ? "," : "") + std::to_string(candidates[ci]);
                DiagLog("SPLIT-PICK id=%d cells=%d minL=%d maxL=%d span=%d pick=%d chosen=%d delta=%d cands=[%s]",
                        nr->id, nr->cellCount, minLevel, maxLevel, span,
                        pick, chosen, delta, cs.c_str());
            }

            CellIterator it;                               // 0x58e3bd
            it.Reset(cellSlots_, size_.mapWidth, slotStride_);
            while (MapCell* c = it.Next())                 // 0x58e3ce
            {
                const int16_t x = static_cast<int16_t>(c->MapCoords & 0xFFFF);
                const int16_t y = static_cast<int16_t>((uint32_t)c->MapCoords >> 16);
                if (workCells_[x + side * y].data[14] == nr->id)
                {
                    const int before = c->Level;
                    c->Level += delta;                     // 0x58e3ef
                    if (delta != 0)
                        DiagLog("SPLIT-SHIFT (%d,%d) L%d->%d id=%d pick",
                                x, y, before, c->Level, nr->id);
                }
            }
        }
    }

    // ---- 8. Destroy the consumed records (0x58e52x - 0x58e55x) ------------
    int nConsumed = 0;
    for (int i = static_cast<int>(regions_.size()) - 1; i >= 0; --i)
    {
        MapRegion* r = regions_[static_cast<size_t>(i)];
        if (r == nullptr || r == region)                  // vanilla keeps v62
            continue;
        if (r->consumed != 0)                             // 0x58e52e
        {
            ++nConsumed;
            ReleaseRegionObject(r, true);                 // sub_58C070 + delete
        }
    }
    DiagLog("SPLIT-END src=%d created=%zu consumed=%d",
            region->id, created.size(), nConsumed);

    // (0x58e561 ++record[7] - the +0x1C split counter - is a write-only field and
    //  is not modelled.)

    return true;                                          // 0x58e5c3 `mov al, 1`
}

// sub_58EF10 - Making-regions step 2: rebuild the region set from the work marks,
// then link the regions and carve the ramps between them.
//
// Vanilla body (58EF10.c, disasm 0x58ef10 - 0x58f0b4):
//   1. 0x58ef15 - 0x58ef65: reset the preview state (0x58ef23 sub_4A8BF0 =
//      ResetPreviewState) and run one diamond pass of sub_579010(cell, -1), the
//      recursive "cliff blob" step (0x58ef50). Its result lands in v15
//      (0x58ef5a) and the loop stops as soon as it turns 0 - but sub_579010's
//      only `return 0` sits behind `mode != -1` (0x579010 `v14 > 0 && v14 != a2
//      && a2 != -1`), so with -1 it always answers 1 and the pass walks every
//      diamond cell.
//   2. 0x58ef67 - 0x58ef96: put work[+56] (data[14], 0x58ef88) and work[+60]
//      (data[15], 0x58ef92) back to -1 over the whole square.
//   3. 0x58ef98 - 0x58efc3: destroy EVERY region in the array, newest first -
//      sub_58C070 (0x58efb4, the destructor body, which also compacts the array)
//      plus operator delete (0x58efba), i.e. exactly
//      ReleaseRegionObject(region, true).
//   4. 0x58efcb - 0x58f02b: re-create a region for every cell the marks still
//      show as unassigned (work[+56] == -1 with non-zero coords) through
//      sub_58C800 (0x58f01b), stamping byte +26 = 0 on each new record
//      (0x58f024). This is byte-identical to the Init-regions leftover pass, so
//      it reuses AssignRemainingCells(0) (sub_58D010).
//   5. 0x58f033 - 0x58f0aa, only when the blob pass answered 1: sub_58F0C0 per
//      region (0x58f04b, build its neighbour-id vector at +4), then sub_5905D0
//      per region (0x58f069, carve the ramps), then free every region's +4
//      vector (0x58f07f - 0x58f0aa).
//
// Between steps 3 and 4, at 0x58efd0, the same function zeroes dword_ABED14 -
// the region-id counter. NOTE: dword_ABED14 is the address the IDB also names
// pFoundationData_ (one global, two names), so this reset and sub_58F0C0's
// neighbour-map size are one counter = regionIdCounter_ (which is also what
// sub_58EF10's caller expects: ids restart from 0).
//
// Returns 1 (0x58f0ae `mov al, 1`); the caller (0x598d67) drops it, hence void
// here.
//
// RNG: steps 1-4 draw nothing (sub_58C800's reset path is the only exception and
// it needs a zero region-id counter to run - true right after 0x58efd0). Step 5
// goes through the ramp subsystem: the land branch rolls 0..100 and 1..2 per
// neighbour pair, the seven builders draw their 0/1 strip picks, and the water
// branch (sub_58F2C0) draws the anchor-cell index plus up to two 0/1 picks per
// attempt.
void RandomMapGenerator::ReseedRegions()
{
    if (workCells_ == nullptr || cellSlots_ == nullptr)
        return;

    const int side = size_.workSide;
    const int workCount = side * side;

    // ---- 1. Preview reset + the cliff blob pass (0x58ef15 - 0x58ef65) -----
    ResetPreviewState();
    // [SNAPSHOT-OFF] SaveStageSnapshot("ResetPreviewState");        // 0x58ef23 sub_4A8BF0
    bool pass = true;                                 // vanilla v15 (0x58ef1e)
    {
        CellIterator it;                              // 0x58ef2d sub_578350
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())             // 0x58ef37 CellIteratorNext
        {
            if (!pass)                                // 0x58ef43
                break;
            pass = CliffPass(cell, -1);               // 0x58ef50 sub_579010
        }
    }
    // [SNAPSHOT-OFF] SaveStageSnapshot("CliffPass");

    // ---- 2. Reset the work marks (0x58ef67 - 0x58ef96) --------------------
    for (int idx = 0; idx < workCount; ++idx)         // 0x58ef7e
    {
        workCells_[idx].data[14] = -1;                // 0x58ef88 byte 56
        workCells_[idx].data[15] = -1;                // 0x58ef92 byte 60
    }
    // [SNAPSHOT-OFF] SaveStageSnapshot("Workmarks");

    // ---- 3. Tear every region down (0x58ef98 - 0x58efc3) ------------------
    // sub_58C070 is sub_5AC290's body without its trailing operator delete
    // (verified against both decompiles), so the vanilla's "sub_58C070(obj);
    // operator delete(obj);" pair is exactly ReleaseRegionObject(obj, true).
    for (int i = static_cast<int>(regions_.size()) - 1; i >= 0; --i)
    {
        MapRegion* region = regions_[static_cast<size_t>(i)];
        if (region != nullptr)                        // 0x58efae
            ReleaseRegionObject(region, true);        // 0x58efb4 + 0x58efba
    }
    regionIdCounter_ = 0;
    // [SNAPSHOT-OFF] SaveStageSnapshot("ReleaseRegionObject");     // 0x58efd0 dword_ABED14

    // ---- 4. Re-seed the leftovers (0x58efcb - 0x58f02b) -------------------
    // The vanilla carries a SECOND inline copy of sub_58D010's sweep here
    // (work[+56] == -1 and non-zero coordinates -> sub_58C800, then byte +26 =
    // the literal 0). The port calls the shared helper with that same flag, so
    // the sweep order and the stamped value are identical to the inlined copy.
    AssignRemainingCells(0);                          // 0x58efcb - 0x58f02b

    // ---- 5. Adjacency and ramps (0x58f033 - 0x58f0aa) ---------------------
    if (pass)                                         // 0x58f038
    {
        for (size_t j = 0; j < regions_.size(); ++j)  // 0x58f04b sub_58F0C0
            LinkRegionNeighbours(regions_[j]);

        // [SNAPSHOT-OFF] SaveStageSnapshot("LinkRegionNeighbours");
        for (size_t j = 0; j < regions_.size(); ++j)
        {// 0x58f069 sub_5905D0
            CarveRegionRamps(regions_[j]); 
            // [SNAPSHOT-OFF] std::string fileName = std::string("CarveRegionRamps") + std::to_string(j);
            // [SNAPSHOT-OFF] const char* name = fileName.c_str();
            // [SNAPSHOT-OFF] SaveStageSnapshot(name);
        }

        // 0x58f07f - 0x58f0aa frees each region's neighbour-id vector (+4) and
        // clears the pointer. Modelled as clearing the vector: sub_58F0C0 built
        // it above and the next stage rebuilds it from scratch.
        for (size_t j = 0; j < regions_.size(); ++j)  // 0x58f07f
            regions_[j]->neighbours.clear();
    }
    // [SNAPSHOT-OFF] SaveStageSnapshot("Neighboursclear");
}

// sub_579010 - one step of the recursive "fill the depressions" pass (579010.c).
//
// The vanilla body (0x579010 - 0x579312):
//   mask = sub_579B70(cell)                       // the height-direction mask
//   hit  = 0
//   if ((mask & 0xA0) == 0xA0) hit = ((mask & 0x11) == 0x11);
//   if (mask & 0x20) { if (CliffMask(west)  & 0x02) hit = 1; }   // W / E facing
//   if (mask & 0x02) { if (CliffMask(east)  & 0x20) hit = 1; }
//   if (mask & 0x08) { if (CliffMask(south) & 0x80) hit = 1; }   // S / N facing
//   if (mask & 0x80) { if (CliffMask(north) & 0x08) hit = 1; }
//   if ((mask & 0x11) == 0x11 && ((mask & 0x0A) != 0x0A || (mask & 0xE0) != 0)
//                             && ((mask & 0xA0) != 0xA0 || (mask & 0x0E) != 0)) hit = 1;
//   if ((mask & 0x44) == 0x44 && ((mask & 0x28) != 0x28 || (mask & 0x83) != 0)
//                             && ((mask & 0x82) != 0x82 || (mask & 0x38) != 0)) hit = 1;
//   if ((mask & 0x2C) == 0x24 || (mask & 0xA1) == 0x21 || (mask & 0x1A) == 0x12
//    || (mask & 0xC2) == 0x42 || (mask & 0x0B) == 0x09 || (mask & 0x68) == 0x48
//    || (mask & 0x86) == 0x84 || (mask & 0xB0) == 0x90) hit = 1;
//   if ((mask & 0x88) == 0x88 || (mask & 0x22) == 0x22 || hit)
//   {
//       mark = work[cell].data[14];                       // sub_5A00C0
//       if (mark > 0 && mark != mode && mode != -1) return 0;
//       cell->Level += 4;                                 // fill the dip
//       work[cell].data[14] = mode;                       // sub_5A0090
//       for (dir = 0; dir < 8; ++dir)
//           sub_579010(GetNeighbourCell(cell, dir), mode);   // recursion
//   }
//   return 1;
//
// The four facing tests fetch the single neighbour directly (the InvalidCell
// fallback applies); for such a cell sub_579B70 answers 0 through its diamond
// gate, which is what CellAt + CliffMask reproduce here.
//
// sub_5A00C0 is a plain work[+56] read and sub_5A0090 its write, so the two are
// inlined below.
bool RandomMapGenerator::CliffPass(MapCell* cell, int mode)
{
    if (cell == nullptr || workCells_ == nullptr)
        return true;

    const int mask = CliffMask(cell);                     // 0x5790xx sub_579B70
    const int x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);

    bool hit = false;
    if ((mask & 0xA0) == 0xA0)
        hit = ((mask & 0x11) == 0x11);

    // The four opposite-side continuation probes (sub_579010 0x57904d ..
    // 0x5791c0, verified against the disassembly, not the pseudocode):
    // when the cell is high on one side, look TWO cells over on the OTHER
    // side - the neighbour past the cell in the opposite direction - and test
    // that neighbour's outward bit. High ground on both sides means this cell
    // is a one-cell dip to fill. The addresses/offsets are:
    //   W bit -> probe (x+1,y), test E  bit 0x02   (lea ecx,[eax+1])
    //   E bit -> probe (x-1,y), test W  bit 0x20   (lea ecx,[eax-1])
    //   S bit -> probe (x,y-1), test N  bit 0x80   (lea eax,[edx-1])
    //   N bit -> probe (x,y+1), test S  bit 0x08   (lea eax,[edx+1])
    if ((mask & 0x20) != 0)                               // W high -> two to the E
    {
        if ((CliffMask(CellAt(static_cast<int16_t>(x + 1), static_cast<int16_t>(y))) & 0x02) != 0)
            hit = true;
    }
    if ((mask & 0x02) != 0)                               // E high -> two to the W
    {
        if ((CliffMask(CellAt(static_cast<int16_t>(x - 1), static_cast<int16_t>(y))) & 0x20) != 0)
            hit = true;
    }
    if ((mask & 0x08) != 0)                               // S high -> two to the N
    {
        if ((CliffMask(CellAt(static_cast<int16_t>(x), static_cast<int16_t>(y - 1))) & 0x80) != 0)
            hit = true;
    }
    if ((mask & 0x80) != 0)                               // N high -> two to the S
    {
        if ((CliffMask(CellAt(static_cast<int16_t>(x), static_cast<int16_t>(y + 1))) & 0x08) != 0)
            hit = true;
    }

    if ((mask & 0x11) == 0x11
        && ((mask & 0x0A) != 0x0A || (mask & 0xE0) != 0)
        && ((mask & 0xA0) != 0xA0 || (mask & 0x0E) != 0))
        hit = true;
    if ((mask & 0x44) == 0x44
        && ((mask & 0x28) != 0x28 || (mask & 0x83) != 0)
        && ((mask & 0x82) != 0x82 || (mask & 0x38) != 0))
        hit = true;
    if ((mask & 0x2C) == 0x24 || (mask & 0xA1) == 0x21
        || (mask & 0x1A) == 0x12 || (mask & 0xC2) == 0x42
        || (mask & 0x0B) == 0x09 || (mask & 0x68) == 0x48
        || (mask & 0x86) == 0x84 || (mask & 0xB0) == 0x90)
        hit = true;

    if ((mask & 0x88) == 0x88 || (mask & 0x22) == 0x22 || hit)
    {
        if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
            return true;                                  // out-of-diamond cell: mask 0
        WorkCell& own = workCells_[x + size_.workSide * y];
        const int mark = own.data[14];                    // 0x5791xx sub_5A00C0
        if (mark > 0 && mark != mode && mode != -1)       // 0x5791xx
            return false;

        cell->Level += 4;                                 // 0x5792xx
        own.data[14] = mode;                              // 0x5792xx sub_5A0090

        static const int16_t kStepX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        static const int16_t kStepY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        for (int dir = 0; dir < 8; ++dir)                 // 0x5792xx
        {
            const int16_t nx = static_cast<int16_t>(x + kStepX[dir]);
            const int16_t ny = static_cast<int16_t>(y + kStepY[dir]);
            CliffPass(CellAt(nx, ny), mode);              // recursion, result dropped
        }
    }
    return true;                                          // 0x57930a
}

// sub_579B70 - the 8-bit height-direction mask (579B70.c, 0x579b70 - 0x57a0b8).
//
//   if (the coords are outside the diamond) return 0;                  // 0x579bxx
//   if (work[cell].byte74 == 0) return 0;                              // the enable flag
//   level = cell->Level                                                // cell +0x11B
//   for d = 0 .. 7 (Neighbours order):
//       n = cell + Neighbours[d]
//       if (CoordinatesLegal(n) && n->Level == level + 4 && !sub_4863D0(n))
//           mask |= 1 << ((d + 7) & 7)
//
// The bit layout comes straight out of the vanilla's per-direction stores:
// SE -> 4, S -> 8, SW -> 0x10, W -> 0x20, NW -> 0x40, N -> 0x80, NE -> 1, E -> 2.
int RandomMapGenerator::CliffMask(const MapCell* cell)
{
    if (cell == nullptr || workCells_ == nullptr)
        return 0;

    const int packed = cell->MapCoords;
    const int16_t x = static_cast<int16_t>(packed & 0xFFFF);
    const int16_t y = static_cast<int16_t>((uint32_t)packed >> 16);
    if (!CellExists(x, y))                                // the diamond gate first
        return 0;
    if (workCells_[x + size_.workSide * y].Byte(74) == 0)  // 0x579bxx
        return 0;

    const int level = cell->Level;
    static const int kBit[8] = { 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40 };

    int mask = 0;
    for (int d = 0; d < 8; ++d)
    {
        const int16_t nx = static_cast<int16_t>(x + kDirX[d]);
        const int16_t ny = static_cast<int16_t>(y + kDirY[d]);
        if (!CellExists(nx, ny))                          // CoordinatesLegal
            continue;
        const MapCell* n = CellAt(nx, ny);
        if (n->Level != level + 4)                        // exactly one level up
            continue;
        if (IsCliffFamilyTile(n))                         // !sub_4863D0(n)
            continue;
        mask |= kBit[d];
    }
    return mask;
}

// sub_4863D0 - the cliff-family tile test (4863D0.c, 0x4863d0 - 0x4865ad).
//
// The tile index (cell +0x38) is tested against the tile families the theater
// INI filled, in this order - the family bases are the globals our port already
// carries:
//   CliffSet          @0xAA1020  40 wide                      shoreTileIndex_
//   WaterfallEast     @0xAA073C   4 wide                      waterFamily4Base_[0]
//   WaterfallWest     @0xABB110   4 wide                      waterFamily4Base_[1]
//   WaterfallSouth    @0xAA1050   4 wide                      waterFamily4Base_[2]
//   WaterfallNorth    @0xAA10A0   4 wide                      waterFamily4Base_[3]
//   CliffRamps        @0xABBEBC  20 wide                      cliffRampsIndex_
//   WaterCaves        @0xABAD24   4 wide                      waterCavesIndex_
//   BridgeSet         @0xAA0E28  16 wide                      bridgeSetIndex_
//   WoodBridgeSet     @0xABAD1C  16 wide                      woodBridgeSetIndex_
//   DestroyableCliffs @0xABC2C8   2 wide                      destroyableCliffsIndex_
//   WaterCliffs       @0xAA101C  28 wide                      waterCliffsIndex_
// A -1 base means "family not loaded" and the test is skipped.
//
// The four waterfall families carry an extra Height test (cell +0x11A) on
// their first and fourth tile (the other two tiles are always accepted):
//   East  (base/base+3): height != 0 && height != 4
//   West  (base/base+3): height != 1 && height != 3
//   South (base/base+3): height >= 2
//   North (base/base+3): height != 2 && height != 3
// Inside any of the other families the answer is a plain true.
//
// The byte is YRpp's CellClass::Height, NOT the SlopeIndex at +0x11C:
// sub_4863D0 reads `mov cl, [ecx+11Ah]` four times, once per family, while
// sub_47D2B0 is what writes +0x11C. (The port read SlopeIndex here until this
// mix-up was found - the two fields are adjacent bytes and the vanilla uses
// both, for different things.)
bool RandomMapGenerator::IsCliffFamilyTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;

    const int tile = cell->IsoTileTypeIndex;             // cell +0x38
    const int height = cell->Height;                     // cell +0x11A

    if (shoreTileIndex_ != -1
        && tile >= shoreTileIndex_ && tile < shoreTileIndex_ + 40)
        return true;

    for (int i = 0; i < 4; ++i)                          // the four waterfalls
    {
        const int base = waterFamily4Base_[i];
        if (base == -1 || tile < base || tile >= base + 4)
            continue;
        if (tile != base && tile != base + 3)
            return true;                                 // the middle two tiles
        switch (i)
        {
        case 0: return height != 0 && height != 4;       // East
        case 1: return height != 1 && height != 3;       // West
        case 2: return height >= 2;                      // South
        default: return height != 2 && height != 3;      // North
        }
    }

    if (cliffRampsIndex_ != -1
        && tile >= cliffRampsIndex_ && tile < cliffRampsIndex_ + 20)
        return true;
    if (waterCavesIndex_ != -1
        && tile >= waterCavesIndex_ && tile < waterCavesIndex_ + 4)
        return true;
    if (bridgeSetIndex_ != -1
        && tile >= bridgeSetIndex_ && tile < bridgeSetIndex_ + 16)
        return true;
    if (woodBridgeSetIndex_ != -1
        && tile >= woodBridgeSetIndex_ && tile < woodBridgeSetIndex_ + 16)
        return true;
    if (destroyableCliffsIndex_ != -1
        && tile >= destroyableCliffsIndex_ && tile < destroyableCliffsIndex_ + 2)
        return true;
    if (waterCliffsIndex_ == -1)
        return false;
    return tile >= waterCliffsIndex_ && tile < waterCliffsIndex_ + 28;
}

// sub_58F0C0 - build one region's neighbour-id vector (58F0C0.c, 0x58f0c0 -
// 0x58f2be). No RNG.
//
//   1. 0x58f0c7 - 0x58f112: allocate the DynamicVectorClass<int> (0x18 bytes,
//      growth 10) and store it at region +4.
//   2. 0x58f114 - 0x58f13a: allocate a byte map of dword_ABED14 (regionIdCounter_)
//      entries and clear it.
//   3. 0x58f13c - 0x58f1d9: take the region's boundary cells (sub_58D410 = our
//      CollectRegionBoundary) and, for each of them, walk the 8 neighbours; every
//      in-diamond neighbour whose work[+56] is >= 0 marks that id in the map.
//   4. 0x58f1db - 0x58f202: append every marked id except the region's own to the
//      vector.
//   5. 0x58f204 - 0x58f248: recount the region's cells (+12): the work square
//      scanned linearly, counting cells whose work[+56] equals the region id and
//      whose coords are non-zero.
//   6. 0x58f24a - 0x58f25a: free the boundary list and the byte map.
void RandomMapGenerator::LinkRegionNeighbours(MapRegion* region)
{
    if (region == nullptr || workCells_ == nullptr)
        return;

    const int side = size_.workSide;
    const int workCount = side * side;

    // 1. the vector at region +4
    region->neighbours.clear();                       // 0x58f0f9

    // 2. the touching-region byte map (indexed by region id)
    std::vector<unsigned char> touching(               // 0x58f114
        static_cast<size_t>(regionIdCounter_), 0);

    // 3. which regions own a cell next to the region's boundary
    const std::vector<CellStruct> boundary = CollectRegionBoundary(region);  // 0x58f13c
    for (size_t i = 0; i < boundary.size(); ++i)       // 0x58f158
    {
        const CellStruct c = boundary[i];
        for (int dir = 0; dir < 8; ++dir)              // 0x58f16d
        {
            const int16_t nx = static_cast<int16_t>(c.X + kDirX[dir]);
            const int16_t ny = static_cast<int16_t>(c.Y + kDirY[dir]);
            if (!CellExists(nx, ny))                   // 0x58f... (the diamond gate)
                continue;
            const int mark = workCells_[nx + side * ny].data[14];
            if (mark >= 0 && static_cast<size_t>(mark) < touching.size())  // 0x58f1a9
                touching[static_cast<size_t>(mark)] = 1;
        }
    }

    // 4. append the ids (skipping this region's own)
    for (int id = 0; id < regionIdCounter_; ++id)      // 0x58f1db
    {
        if (touching[static_cast<size_t>(id)] != 0 && id != region->id)
            region->neighbours.push_back(id);          // 0x58f1f6
    }

    // 5. recount the region's cells
    region->cellCount = 0;                             // 0x58f204
    for (int idx = 0; idx < workCount; ++idx)          // 0x58f20a
    {
        if (workCells_[idx].data[14] == region->id
            && workCells_[idx].data[0] != 0)           // coords non-zero
            ++region->cellCount;
    }
}

// sub_5905D0 - carve the ramps between one region and its neighbours (5905D0.c,
// 0x5905d0 - 0x590967).
//
// Two branches:
//   - region +20 (waterFamily) != 0 (0x5905f7 - 0x590696): walk the neighbour
//     vector; for every neighbour A that is "big" (more than one neighbour or
//     more than 50 cells) scan the vector again from A's own slot (the vanilla's
//     inner index starts AT the outer one, so A pairs with itself too) and, for
//     every B that is big and not a water family and whose Level equals both A's
//     and this region's, call sub_58F2C0(thisRegion, A, B). No RNG in this branch.
//   - waterFamily == 0 (0x5906a6 - 0x590a5c): for every neighbour with a HIGHER
//     ID (the pair guard, so each unordered pair is handled once) and a different
//     Level: roll the ramp count [RNG, 0..100] and, when it is below the global
//     n0x64 @0xABE044 (the RMG's rolled this[27] = globalOptions_.accessibility),
//     draw 1..2 [RNG] and carve `ramps + 1` of them. The boundary cells of the
//     HIGHER
//     region (sub_58D410) are the try pool: pick one at random [RNG], and when it
//     lies in the usable area call sub_590970(coords, lowerRegionId, tries*0.01);
//     a non-zero answer counts as carved. The loop gives up after 100 tries.
//     When nothing was carved the record's byte +27 is cleared.
//
// The caller runs this for every region right after LinkRegionNeighbours, so the
// +4 vector is populated.
void RandomMapGenerator::CarveRegionRamps(MapRegion* region)
{
    if (region == nullptr || workCells_ == nullptr)
        return;

    if (region->waterFamily)                              // 0x5905f7
    {
        const std::vector<int> nb = region->neighbours;   // a copy: callees can
        const int count = static_cast<int>(nb.size());    // reshape the array
        if (count - 1 > 0)                                // 0x59060c
        {
            for (int i = 1; i < count; ++i)               // 0x5906xx (v42)
            {
                MapRegion* a = FindRegionById(nb[static_cast<size_t>(i)]);
                if (a == nullptr)                         // vanilla reads past the
                    continue;                             // array here
                if (a->neighbours.size() <= 1 && a->cellCount <= 50)   // 0x5906xx
                    continue;                             // LABEL_28
                for (int j = i; j < count; ++j)           // 0x5907xx (j starts at i)
                {
                    MapRegion* b = FindRegionById(nb[static_cast<size_t>(j)]);
                    if (b == nullptr)
                        continue;
                    if ((b->neighbours.size() > 1 || b->cellCount > 50)
                        && !b->waterFamily)               // 0x5907xx
                    {
                        if (b->level == a->level && a->level == region->level)
                            LinkSameLevelRegions(region, a, b);   // 0x5906d5 sub_58F2C0
                    }
                }
            }
        }
        return;
    }

    const std::vector<int> nb = region->neighbours;       // 0x5906a6
    for (size_t k = 0; k < nb.size(); ++k)
    {
        MapRegion* other = FindRegionById(nb[k]);         // 0x5906xx
        if (other == nullptr)
            continue;
        if (other->id <= region->id                        // 0x5906xx the pair guard
            || other->level == region->level)
            continue;

        int attempts = 0;                                 // vanilla's n100 / v44
        int carved = 0;                                   // vanilla's v40

        unsigned int roll;
        do
        {
            roll = static_cast<unsigned int>(F2I64(       // 0x5907xx [RNG] 0..100
                static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                * 0.00000002351589501451605));
        }
        while (roll > 100);

        unsigned int ramps = 0;
        const unsigned int gate =                         // n0x64 = this[27]
            static_cast<unsigned int>(globalOptions_.accessibility);
        if (roll >= gate)
        {
            ramps = 0;
        }
        else
        {
            do
            {
                ramps = static_cast<unsigned int>(F2I64(  // 0x5907xx [RNG] 1..2
                    static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                    * 4.656612874161595e-10 + 1.0));
            }
            while (ramps > 2);
        }
        const int want = static_cast<int>(ramps) + 1;     // 0x5907xx (v45)

        const bool regionIsHigher = other->level <= region->level;   // 0x5907xx
        const int higherId = regionIsHigher ? region->id : other->id;
        const int lowerId  = regionIsHigher ? other->id : region->id;

        MapRegion* higher = FindRegionById(higherId);     // 0x5907xx
        if (higher == nullptr)
            continue;
        const std::vector<CellStruct> list = CollectRegionBoundary(higher);  // 0x5908cd

        // Port tuning (user request 2026-10-01): the roll gate above
        // (accessibility) already grants 2-3 ramp slots for most pairs; the
        // vanilla 100-try sampling budget is kept on purpose. A test with 200
        // tries did reach more boundary points, but the late-sampled strips
        // landed with their L4 band foot right against a DIFFERENT cliff
        // facade (ramps are carved before PlaceCliffs, so the strip clearance
        // cannot see that later cliff) - producing two-level ramp/cliff
        // clashes. Staying at 100 avoids those while the raised gate still
        // boosts ramp frequency wherever the geometry is clean.
        const int kMaxRampAttempts = 100;
        while (carved < want)
        {
            if (attempts >= kMaxRampAttempts)
                break;
            if (!list.empty())
            {
                int idx;
                do
                {
                    idx = F2I64(                          // 0x5909xx [RNG]
                        static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                        * static_cast<double>(list.size()) * kUnitScale);
                }
                while (idx > static_cast<int>(list.size()) - 1);

                const CellStruct c = list[static_cast<size_t>(idx)];
                if (IsWithinUsableArea(c, true))          // 0x59099x
                    carved += (CarveRampAt(region, c, lowerId,
                                           attempts * 0.01f) != 0);  // 0x590970
            }
            ++attempts;                                   // v44 = ++n100
        }

        if (carved == 0)
            region->rampFlag = 0;                         // 0x590a4f (byte +27)
    }
}

// sub_590970 - try to carve one ramp at the given cell (590970.c, 0x590970 -
// 0x590fc8).
//
// The body is a dispatcher:
//   flag = 1; mask = sub_590FD0(coords, &flag, regionId, ratio)
//   if (!flag) return 0;                       // 0x59097e
//   if (mask == 255) return 0;                 // 0x59098a
// then it walks a fixed cascade of (mask pattern -> one directed builder) pairs,
// each tried only while the previous ones failed:
//   (mask & 0x82) == 0x82 && (mask & 0x38) == 0   -> sub_593AF0  strip (X-1,Y-5)-(X+5,Y+1),
//                                                     Y+1 more when mask & 0x40,
//                                                     X-1 less when mask & 4
//   (mask & 0x0A) == 0x0A && (mask & 0x11) == 0   -> sub_593550  strip (X+5,Y-1)-(X-1,Y+5),
//                                                     Y-1 less when mask & 0x10,
//                                                     X-1 less when mask & 1
//   (mask & 0x28) == 0x28 && (mask & 0x83) == 0   -> sub_593030  strip (X+1,Y+5)-(X-5,Y-1),
//                                                     X-1 less when mask & 0x40,
//                                                     Y-1 less when mask & 4
//   (mask & 0xA0) == 0xA0 && (mask & 0x0E) == 0   -> sub_593030  strip (X-5,Y+1)-(X+1,Y-5),
//                                                     Y+1 more when mask & 1,
//                                                     X+1 more when mask & 0x10
//   (mask & 0x70) == 0    && (mask & 0x88) != 0   -> sub_592440  strip (X-1,Y-4)-(X-1,Y+4)
//                                                     (+1 / -1 on Y when mask & 0x80 / 8),
//                                                     then two 0/1 draws add to both X  [RNG x2]
//   (mask & 0x07) == 0    && (mask & 0x88) != 0   -> sub_591740  strip (X+1,Y+4)-(X+1,Y-4)
//                                                     (+1 / -1 on Y when mask & 0x80 / 8),
//                                                     then two 0/1 draws give X += r - 1 [RNG x2]
//   (mask & 0xC1) == 0    && (mask & 0x22) != 0   -> sub_591D80  strip (X+4,Y-1)-(X-4,Y-1)
//                                                     (+1 on X when mask & 0x20, -1 when mask & 2),
//                                                     then two 0/1 draws add to both Y  [RNG x2]
//   (mask & 0x1C) == 0    && (mask & 0x22) != 0   -> sub_5910F0  strip (X-4,Y+1)-(X+4,Y+1)
//                                                     (+1 on X when mask & 0x20, -1 when mask & 2),
//                                                     then two 0/1 draws give Y += r - 1 [RNG x2]
// When it still failed and `ratio` (the caller's try counter * 0.01) is above 0.5
// it makes up to four more attempts with fixed strips, in this order and gated on
// the mask, returning the last builder's answer:
//   (mask & 8) == 0  -> sub_5910F0 on (X-4,Y+1)-(X+4,Y+1)
//   (mask & 2) == 0  -> sub_591740 on (X+1,Y+4)-(X+1,Y-4)
//   (mask & 8) == 0  -> sub_591D80 on (X+4,Y-1)-(X-4,Y-1)
//   (mask & 0x20) == 0 -> sub_592440 on (X-1,Y-4)-(X-1,Y+4)
// The 0/1 draws are F2I64(Random() * 2^-31) (4.656612874161595e-10), i.e. the
// low half of kUnitScale; the rejection loops cannot repeat. Eight RNG draws in
// total on the deepest path.
//
// Callees still missing: sub_590FD0 (RampMask) and the seven builders.
int RandomMapGenerator::CarveRampAt(MapRegion* owner, CellStruct coords, int regionId,
                                   float ratio)
{
    const int x = coords.X;
    const int y = coords.Y;

    bool ok = true;                                       // 0x590974 (flag[0] = 1)
    const int mask = RampMask(coords, ok, regionId, ratio);   // 0x590981 sub_590FD0
    if (!ok)                                              // 0x59097e
        return 0;
    if (mask == 255)                                      // 0x59098a
        return 0;

    int carved = 0;                                       // vanilla's v7

    // ------------------------------------------------------------------
    // 刻完回头验（用户报障 2026-10-02：(94,126) 一带悬崖堵塞）
    //
    // 坡肩（builder 靠 owner 那一侧的台阶）是刻坡过程中才抬出来的高地，
    // 刻坡前根本不存在，所以 RampRectClear 那种"刻坡前量距离"的静态检查
    // 永远量不到它。这里改成刻完之后回头看一眼：owner 自己的高地格
    // （坡肩加本体）有没有被这次刻坡切成两块。切开了就说明坡肩和本体之间
    // 只剩一条窄缝，落崖时两边崖壁挤在一起、甚至挤出没有崖壁的新落差
    // （(94,128) 就是这样），于是把这次刻坡整块撤销，让 CarveRegionRamps
    // 的选点循环去试下一个候选点。
    // ------------------------------------------------------------------
    const int side = size_.workSide;
    static const int kDir8X[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int kDir8Y[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    const int snapX0 = x - 20;                            // 本次刻坡会改到的范围
    const int snapY0 = y - 20;
    const int snapW = 41;
    const int snapH = 41;

    // owner 高地格（编号是 owner、Level 不低于 owner->level 的格子）被分成了
    // 几块；只数跟快照窗口有交的块，窗口外那些本来就有的旧块不算数。
    auto countOwnerHighBlocks = [&]() -> int
    {
        const int scanX0 = x - 28;                        // 比快照窗口再外扩 8 格
        const int scanY0 = y - 28;
        const int scanW = 57;
        const int scanH = 57;
        std::vector<unsigned char> seen(static_cast<size_t>(scanW) * scanH, 0);
        std::vector<int> stack;
        int blocks = 0;
        for (int row = 0; row < scanH; ++row)
        {
            for (int col = 0; col < scanW; ++col)
            {
                const int cx = scanX0 + col;
                const int cy = scanY0 + row;
                if (seen[row * scanW + col] || !CellExists(cx, cy))
                    continue;
                if (workCells_[cx + side * cy].data[14] != owner->id)
                    continue;
                const MapCell* mc = CellAt(cx, cy);
                if (mc == nullptr || mc->Level < owner->level)
                    continue;

                bool touchesSnapshot = false;
                ++blocks;
                seen[row * scanW + col] = 1;
                stack.push_back(row * scanW + col);
                while (!stack.empty())
                {
                    const int idx = stack.back();
                    stack.pop_back();
                    const int px = scanX0 + idx % scanW;
                    const int py = scanY0 + idx / scanW;
                    if (px >= snapX0 && px < snapX0 + snapW
                        && py >= snapY0 && py < snapY0 + snapH)
                        touchesSnapshot = true;
                    for (int d = 0; d < 8; ++d)
                    {
                        const int nx = px + kDir8X[d];
                        const int ny = py + kDir8Y[d];
                        if (!CellExists(nx, ny))
                            continue;
                        const int nc = nx - scanX0;
                        const int nr = ny - scanY0;
                        if (nc < 0 || nc >= scanW || nr < 0 || nr >= scanH)
                            continue;
                        const int nidx = nr * scanW + nc;
                        if (seen[nidx])
                            continue;
                        if (workCells_[nx + side * ny].data[14] != owner->id)
                            continue;
                        const MapCell* nmc = CellAt(nx, ny);
                        if (nmc == nullptr || nmc->Level < owner->level)
                            continue;
                        seen[nidx] = 1;
                        stack.push_back(nidx);
                    }
                }
                if (!touchesSnapshot)
                    --blocks;                             // 窗口外的旧块不计入
            }
        }
        return blocks;
    };

    const int ownerHighBlocksBefore = countOwnerHighBlocks();

    std::vector<MapCell> snapCells(static_cast<size_t>(snapW) * snapH);
    std::vector<unsigned char> snapHas(static_cast<size_t>(snapW) * snapH, 0);
    std::vector<int> snapMarks(static_cast<size_t>(snapW) * snapH, 0);
    for (int row = 0; row < snapH; ++row)
    {
        for (int col = 0; col < snapW; ++col)
        {
            const int cx = snapX0 + col;
            const int cy = snapY0 + row;
            if (!CellExists(cx, cy))
                continue;
            const size_t i = static_cast<size_t>(row) * snapW + col;
            snapHas[i] = 1;
            snapMarks[i] = workCells_[cx + side * cy].data[14];
            if (MapCell* mc = RawSlot(cx + (cy * slotStride_)))
                snapCells[i] = *mc;
        }
    }

    auto restoreSnapshot = [&]()
    {
        for (int row = 0; row < snapH; ++row)
        {
            for (int col = 0; col < snapW; ++col)
            {
                const size_t i = static_cast<size_t>(row) * snapW + col;
                if (!snapHas[i])
                    continue;
                const int cx = snapX0 + col;
                const int cy = snapY0 + row;
                workCells_[cx + side * cy].data[14] = snapMarks[i];
                if (MapCell* mc = RawSlot(cx + (cy * slotStride_)))
                    *mc = snapCells[i];
            }
        }
    };

    // ------------------------------------------------------------------
    // 刻完回头验②（用户报障 2026-10-02：(89,116) 与 (92,116) 距离不够展开
    // 两套 CliffSet，西木同款 bug）。
    //
    // 一条对接刻坡由三条坡带组成（R1..R10 = SlopeIndex 1..10）。坡带顶排
    // 贴着 owner 高台（坡格 Level = ownerLevel-1，其上沿即 Level=ownerLevel
    // 的纯高地顶面）。若某条坡带格的"侧向"（垂直于坡带走向、朝高台那一侧）
    // 两格以内存在一个【刻坡前就已是、刻坡后仍是】的等高高地顶面格
    // （SlopeIndex==0、Level == 坡格 Level+1），说明坡顶与旁边另一片高地
    // 贴得太近——之后 PlaceCliffs 会在中间再补一道悬崖，两套 CliffSet 在
    // 两三格内展不开，拼出白立面。命中就整块还原（三条对接坡带一起取消）。
    //
    // slope 朝向（用户给定，俯视）：
    //   R1/R5 -> 左上(-1,-1)   R2/R7 -> 右下(+1,+1)
    //   R3/R10-> 左下(-1,+1)   R4/R8 -> 右上(+1,-1)
    // ------------------------------------------------------------------
    auto sideHighClash = [&](int& clashX, int& clashY, int& clashSlope) -> bool
    {
        if (rampBaseIndex_ < 0)
            return false;
        static const int kSx[11] =
            { 0, -1, 1, -1, 1, -1, 0, 1, 1, 0, -1 };
        static const int kSy[11] =
            { 0, -1, 1, 1, -1, -1, 0, 1, -1, 0, 1 };

        for (int row = 0; row < snapH; ++row)
        {
            for (int col = 0; col < snapW; ++col)
            {
                const size_t i = static_cast<size_t>(row) * snapW + col;
                if (!snapHas[i])
                    continue;

                const MapCell& oldC = snapCells[i];
                const int wx = snapX0 + col;
                const int wy = snapY0 + row;
                MapCell* nowC = RawSlot(wx + (wy * slotStride_));
                if (nowC == nullptr)
                    continue;

                // 仅看"本次新刻"的坡带格：刻前不是 rampBase 坡，刻后是。
                const bool wasRamp =
                    oldC.IsoTileTypeIndex >= rampBaseIndex_
                    && oldC.IsoTileTypeIndex <= rampBaseIndex_ + 19;
                const bool isRamp =
                    nowC->IsoTileTypeIndex >= rampBaseIndex_
                    && nowC->IsoTileTypeIndex <= rampBaseIndex_ + 19;
                if (wasRamp || !isRamp || nowC->SlopeIndex < 1
                    || nowC->SlopeIndex > 10)
                    continue;

                const int slope = nowC->SlopeIndex;
                const int sdx = kSx[slope];
                const int sdy = kSy[slope];
                const int needLevel = nowC->Level + 1;   // 坡格上沿高台高度

                for (int step = 1; step <= 2; ++step)
                {
                    const int qx = wx + sdx * step;
                    const int qy = wy + sdy * step;
                    const int qc = qx - snapX0;
                    const int qr = qy - snapY0;
                    if (qc < 0 || qc >= snapW || qr < 0 || qr >= snapH)
                        break;
                    const size_t qi = static_cast<size_t>(qr) * snapW + qc;
                    if (!snapHas[qi])
                        break;
                    MapCell* qNow = RawSlot(qx + (qy * slotStride_));
                    if (qNow == nullptr)
                        break;
                    const MapCell& qOld = snapCells[qi];

                    // 刻前即纯高地顶面、等高；刻后仍未被本次刻坡改动（依旧
                    // 是同高度纯顶面）——即"旁边另一片没被纳入的高地"。
                    if (qOld.SlopeIndex == 0 && qOld.Level == needLevel
                        && qNow->SlopeIndex == 0 && qNow->Level == needLevel)
                    {
                        clashX = qx;
                        clashY = qy;
                        clashSlope = slope;
                        return true;
                    }
                }
            }
        }
        return false;
    };

    // builder 报成功之后过一遍上面的检查；不合格就整块还原，当这次没刻过。
    auto acceptBuilder = [&](int builderResult) -> int
    {
        if (builderResult == 0)
            return 0;

        int clashX = 0, clashY = 0, clashSlope = 0;
        if (sideHighClash(clashX, clashY, clashSlope))
        {
            DiagLog("RAMP-SIDE-CANCEL near=(%d,%d) R%d ownerL=%d",
                    clashX, clashY, clashSlope, owner->level);
            restoreSnapshot();
            return 0;
        }

        if (countOwnerHighBlocks() <= ownerHighBlocksBefore)
            return builderResult;
        restoreSnapshot();
        return 0;
    };

    CellStruct from;
    CellStruct to;

    if (!carved && (mask & 0x82) == 0x82 && (mask & 0x38) == 0)
    {
        from = CellStruct{ static_cast<int16_t>(x - 1), static_cast<int16_t>(y - 5) };
        to   = CellStruct{ static_cast<int16_t>(x + 5), static_cast<int16_t>(y + 1) };
        if ((mask & 0x40) != 0)
            ++from.Y;                                     // 0x5909xx
        if ((mask & 0x04) != 0)
            --to.X;
        carved = acceptBuilder(RampBuilder1(owner, from, to, regionId));        // 0x590a0x sub_593AF0
    }

    if (!carved && (mask & 0x0A) == 0x0A && (mask & 0x11) == 0)
    {
        from = CellStruct{ static_cast<int16_t>(x + 5), static_cast<int16_t>(y - 1) };
        to   = CellStruct{ static_cast<int16_t>(x - 1), static_cast<int16_t>(y + 5) };
        if ((mask & 0x10) != 0)
            --to.Y;
        if ((mask & 0x01) != 0)
            --from.X;
        carved = acceptBuilder(RampBuilder2(owner, from, to, regionId));        // 0x590a5x sub_593550
    }

    if (!carved && (mask & 0x28) == 0x28 && (mask & 0x83) == 0)
    {
        from = CellStruct{ static_cast<int16_t>(x + 1), static_cast<int16_t>(y + 5) };
        to   = CellStruct{ static_cast<int16_t>(x - 5), static_cast<int16_t>(y - 1) };
        if ((mask & 0x40) != 0)
            --to.X;
        if ((mask & 0x04) != 0)
            --from.Y;
        carved = acceptBuilder(RampBuilder3(owner, from, to, regionId));        // 0x590axx sub_593030
    }

    if (!carved && (mask & 0xA0) == 0xA0 && (mask & 0x0E) == 0)
    {
        from = CellStruct{ static_cast<int16_t>(x - 5), static_cast<int16_t>(y + 1) };
        to   = CellStruct{ static_cast<int16_t>(x + 1), static_cast<int16_t>(y - 5) };
        if ((mask & 0x01) != 0)
            ++to.Y;
        if ((mask & 0x10) != 0)
            ++from.X;
        carved = acceptBuilder(RampBuilder3(owner, from, to, regionId));        // 0x590bxx sub_593030
    }

    if (!carved && (mask & 0x70) == 0 && (mask & 0x88) != 0)
    {
        from = CellStruct{ static_cast<int16_t>(x - 1), static_cast<int16_t>(y - 4) };
        to   = CellStruct{ static_cast<int16_t>(x - 1), static_cast<int16_t>(y + 4) };
        if ((mask & 0x80) != 0)
        {
            ++from.Y;
            ++to.Y;
        }
        if ((mask & 0x08) != 0)
        {
            --to.Y;
            --from.Y;
        }
        from.X = static_cast<int16_t>(from.X + DrawZeroOrOne());   // 0x5909xx [RNG]
        to.X   = static_cast<int16_t>(to.X + DrawZeroOrOne());     // [RNG]
        carved = acceptBuilder(RampBuilder4(owner, from, to, regionId));        // 0x590axx sub_592440
    }

    if (!carved && (mask & 0x07) == 0 && (mask & 0x88) != 0)
    {
        from = CellStruct{ static_cast<int16_t>(x + 1), static_cast<int16_t>(y + 4) };
        to   = CellStruct{ static_cast<int16_t>(x + 1), static_cast<int16_t>(y - 4) };
        if ((mask & 0x80) != 0)
        {
            ++to.Y;
            ++from.Y;
        }
        if ((mask & 0x08) != 0)
        {
            --from.Y;
            --to.Y;
        }
        from.X = static_cast<int16_t>(from.X + DrawZeroOrOne() - 1);   // 0x590bxx [RNG]
        to.X   = static_cast<int16_t>(to.X + DrawZeroOrOne() - 1);     // [RNG]
        carved = acceptBuilder(RampBuilder6(owner, from, to, regionId));        // 0x590bxx sub_591740
    }

    if (!carved && (mask & 0xC1) == 0 && (mask & 0x22) != 0)
    {
        from = CellStruct{ static_cast<int16_t>(x + 4), static_cast<int16_t>(y - 1) };
        to   = CellStruct{ static_cast<int16_t>(x - 4), static_cast<int16_t>(y - 1) };
        if ((mask & 0x20) != 0)
        {
            ++to.X;
            ++from.X;
        }
        if ((mask & 0x02) != 0)
        {
            --from.X;
            --to.X;
        }
        from.Y = static_cast<int16_t>(from.Y + DrawZeroOrOne());   // 0x590cxx [RNG]
        to.Y   = static_cast<int16_t>(to.Y + DrawZeroOrOne());     // [RNG]
        carved = acceptBuilder(RampBuilder5(owner, from, to, regionId));        // 0x590cxx sub_591D80
    }

    if (!carved && (mask & 0x1C) == 0 && (mask & 0x22) != 0)
    {
        from = CellStruct{ static_cast<int16_t>(x - 4), static_cast<int16_t>(y + 1) };
        to   = CellStruct{ static_cast<int16_t>(x + 4), static_cast<int16_t>(y + 1) };
        if ((mask & 0x20) != 0)
        {
            ++from.X;
            ++to.X;
        }
        if ((mask & 0x02) != 0)
        {
            --to.X;
            --from.X;
        }
        from.Y = static_cast<int16_t>(from.Y + DrawZeroOrOne() - 1);   // 0x590cxx [RNG]
        to.Y   = static_cast<int16_t>(to.Y + DrawZeroOrOne() - 1);     // [RNG]
        carved = acceptBuilder(RampBuilder7(owner, from, to, regionId));        // 0x590cxx sub_5910F0
    }

    // The second sweep the vanilla runs only for the later tries (ratio > 0.5).
    if (ratio > 0.5f && carved == 0)                      // 0x590d7x
    {
        if ((mask & 0x08) == 0)                           // the mask skips this strip
        {
            from = CellStruct{ static_cast<int16_t>(x - 4), static_cast<int16_t>(y + 1) };
            to   = CellStruct{ static_cast<int16_t>(x + 4), static_cast<int16_t>(y + 1) };
            carved = acceptBuilder(RampBuilder7(owner, from, to, regionId));    // 0x590dxx sub_5910F0
        }

        if (carved == 0)
        {
            if ((mask & 0x02) == 0)
            {
                from = CellStruct{ static_cast<int16_t>(x + 1), static_cast<int16_t>(y + 4) };
                to   = CellStruct{ static_cast<int16_t>(x + 1), static_cast<int16_t>(y - 4) };
                carved = acceptBuilder(RampBuilder6(owner, from, to, regionId));   // 0x590dxx sub_591740
            }

            if (carved == 0)
            {
                if ((mask & 0x08) == 0)
                {
                    from = CellStruct{ static_cast<int16_t>(x + 4), static_cast<int16_t>(y - 1) };
                    to   = CellStruct{ static_cast<int16_t>(x - 4), static_cast<int16_t>(y - 1) };
                    carved = acceptBuilder(RampBuilder5(owner, from, to, regionId));   // 0x590dxx sub_591D80
                }

                if (carved == 0 && (mask & 0x20) == 0)
                {
                    from = CellStruct{ static_cast<int16_t>(x - 1), static_cast<int16_t>(y - 4) };
                    to   = CellStruct{ static_cast<int16_t>(x - 1), static_cast<int16_t>(y + 4) };
                    carved = acceptBuilder(RampBuilder4(owner, from, to, regionId)); // 0x590fxx sub_592440
                    return carved;
                }
            }
        }
    }

    return carved;
}

// F2I64(Random() * 4.656612874161595e-10) - the 0/1 draw sub_590970 jitters the
// ramp strips with (the value is below 2, so the vanilla's rejection loop never
// repeats).
int RandomMapGenerator::DrawZeroOrOne()
{
    return F2I64(static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                 * 4.656612874161595e-10);
}

// sub_590FD0 - the ramp direction mask (590FD0.c, 0x590fd0 - 0x5910e6).
//
//   threshold = F2I64((15.0 - 5.0) * (1.0 - ratio) + 5.0)     // 0x590fd3, 15 down to 5
//   if (!sub_5A1E50(rect(X-2, Y-2, 5, 5), regionId, threshold))   // 0x591043
//   {                                                             // 0x591052
//       *ok = 0;
//       return 0;
//   }
//   mask = 0;
//   for (i = 0; i < 3; ++i)                 // the sample row, Y runs y-7 + 5*i
//     for (j = 0; j < 3; ++j)               // the sample column, X runs x-7 + 5*j
//     {
//       if (i == 1 && j == 1) continue;     // 0x591074, the centre is skipped
//       if (sub_5A1E50(rect(x-7+5j, y-7+5i, 5, 5), regionId, threshold))
//           mask |= dword_82AF18[3*i + j];  // 0x5910bd
//     }
//   return mask;
//
// The 3x3 table dword_82AF18 (read out of the image) is the direction-bit map of
// the same layout CliffMask uses:
//        [0] 0x40 0x80 0x01     NW  N  NE
//        [1] 0x20 0x00 0x02     W   -  E
//        [2] 0x10 0x08 0x04     SW  S  SE
// so the mask says "these neighbouring ramp modules at 5-cell spacing are filled
// by the region". The spacing 5 is also the module width the strips in
// CarveRampAt use.
int RandomMapGenerator::RampMask(CellStruct coords, bool& ok, int regionId, double ratio)
{
    const int threshold = F2I64((15.0 - 5.0) * (1.0 - ratio) + 5.0);   // 0x590ff5

    if (!RectHasRegionCells(coords.X - 2, coords.Y - 2, 5, 5,   // 0x591031
                            regionId, threshold))
    {
        ok = false;                                        // 0x591052
        return 0;
    }

    static const int kSampleBit[9] = { 0x40, 0x80, 0x01,
                                       0x20, 0x00, 0x02,
                                       0x10, 0x08, 0x04 };

    int mask = 0;
    for (int i = 0; i < 3; ++i)                            // 0x59105f
    {
        for (int j = 0; j < 3; ++j)                        // 0x59106a
        {
            if (i == 1 && j == 1)                          // 0x591074
                continue;
            const int sx = coords.X - 7 + 5 * j;           // 0x59106c / 0x5910cb
            const int sy = coords.Y - 7 + 5 * i;           // 0x59108b
            if (RectHasRegionCells(sx, sy, 5, 5, regionId, threshold))   // 0x5910a9
                mask |= kSampleBit[3 * i + j];             // 0x5910bd dword_82AF18
        }
    }
    return mask;
}

// sub_5A1E50 - "does the region own enough of this rectangle?" (0x5a1e50 - 0x5a1f6d)
//
//   count = 0;
//   for (row = 0; row < h; ++row)
//     for (col = 0; col < w; ++col)
//     {
//       x' = x + col; y' = y + row;
//       if (x' + y' > W' && x' - y' < W' && y' - x' < W' && x' + y' <= W' + 2H')
//       {
//           mark = work[x' + workSide * y'].data[14];     // -1 when the array is absent
//           if (mark == regionId && ++count >= threshold)
//               return true;
//       }
//     }
//   return false;
//
// W' is dword_ABED04 and W' + 2H' is dword_ABED08, i.e. the same diamond window
// CellExists tests.
bool RandomMapGenerator::RectHasRegionCells(int x, int y, int w, int h,
                                            int regionId, int threshold)
{
    if (workCells_ == nullptr || h <= 0)
        return false;

    const int side = size_.workSide;
    int count = 0;
    for (int row = 0; row < h; ++row)                      // 0x5a1e5x
    {
        for (int col = 0; col < w; ++col)
        {
            const int cx = x + col;
            const int cy = y + row;
            const int sum = cx + cy;
            if (sum <= size_.mapWidth                       // the diamond window
                || cx - cy >= size_.mapWidth
                || cy - cx >= size_.mapWidth
                || sum > size_.mapWidth + 2 * size_.mapHeight)
                continue;
            const int mark = workCells_[cx + side * cy].data[14];
            if (mark == regionId && ++count >= threshold)   // 0x5a1f2x
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The seven directed ramp builders, dispatched by CarveRampAt (sub_590970):
// RampBuilder1..7 / sub_593AF0, sub_593550, sub_593030, sub_592440, sub_591D80,
// sub_591740, sub_5910F0.
// ---------------------------------------------------------------------------

// sub_58D070 - write the region-id mark (work[+56]) of one cell (0x58d070, 39
// bytes). Identical to sub_5A0090, which CliffPass inlines.
void RandomMapGenerator::SetWorkMark(CellStruct coords, int value)
{
    if (workCells_ == nullptr)
        return;
    workCells_[coords.X + size_.workSide * coords.Y].data[14] = value;   // 0x58d08a
}

// sub_594010 - the ramp builders' coverage precondition (594010.c, 0x594010 -
// 0x594183).
//
//   if (!IsWithinUsableArea(X,         Y        )) return 0;   // the four corners
//   if (!IsWithinUsableArea(X + w - 1, Y        )) return 0;
//   if (!IsWithinUsableArea(X,         Y + h - 1)) return 0;
//   if (!IsWithinUsableArea(X + w - 1, Y + h - 1)) return 0;
//   for (row = Y; row < Y + h; ++row)
//     for (col = X; col < X + w; ++col)
//     {
//         mark = work[col + workSide * row].data[14];        // -1 when absent
//         if (mark != owner->id && mark != targetId) return 0;
//         if (!sub_486380(the cell))                 return 0;   // placeholder test
//     }
//   return 1;
//
// The vanilla walks the rectangle row by row and leaves through the outer loop as
// soon as a cell fails either test, i.e. any foreign cell rejects the whole block.
bool RandomMapGenerator::RampRectClear(MapRegion* owner, int x, int y, int w, int h,
                                       int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return false;

    const CellStruct c0{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
    const CellStruct c1{ static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y) };
    const CellStruct c2{ static_cast<int16_t>(x), static_cast<int16_t>(y + h - 1) };
    const CellStruct c3{ static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + h - 1) };
    if (!IsWithinUsableArea(c0, true) || !IsWithinUsableArea(c1, true)
        || !IsWithinUsableArea(c2, true) || !IsWithinUsableArea(c3, true))
        return false;

    const int side = size_.workSide;
    for (int row = y; row < y + h; ++row)
    {
        for (int col = x; col < x + w; ++col)
        {
            const int mark = workCells_[col + side * row].data[14];
            if (mark != owner->id && mark != targetId)      // 0x5941xx
                return false;
            if (!IsPlaceholderTile(CellAt(static_cast<int16_t>(col),
                                          static_cast<int16_t>(row))))
                return false;                               // sub_486380
        }
    }

    // Port tuning (user reports 2026-10-01): reject a strip whose footprint
    // sits within TWO cells of a THIRD region's plateau that is >= 2 levels
    // higher than the ramp's low end. Ramps are carved before PlaceCliffs, so
    // sub_594010's in-block mark check cannot see cliffs that later land
    // around the strip; a CliffSet tile renders only ONE facade level, so an
    // L4 band within facade range of an L8 cliff leaves the second level as a
    // white triangle (observed up to two rows away, e.g. rmg_090008 where a
    // 386 band and a later cliff23 collided). One-level ramp-top touches (L7
    // vs L8) and the owner/target regions stay accepted.
    {
        const MapRegion* tgt = FindRegionById(targetId);
        const int lowLevel = (tgt != nullptr) ? tgt->level : owner->level - 1;
        const int highEnough = lowLevel + 2;
        for (int row = y - 2; row <= y + h + 1; ++row)
        {
            for (int col = x - 2; col <= x + w + 1; ++col)
            {
                if (!CellExists(static_cast<int16_t>(col),
                                static_cast<int16_t>(row)))
                    continue;
                const int m = workCells_[col + side * row].data[14];
                if (m <= 0 || m == owner->id || m == targetId)
                    continue;
                const MapCell* mc = CellAt(static_cast<int16_t>(col),
                                           static_cast<int16_t>(row));
                if (mc != nullptr && mc->Level >= highEnough)
                    return false;
            }
        }
    }
    return true;
}

// sub_593AF0 - the first ramp builder: the module stamping plus the three ramp
// bands of one ramp (593AF0.c, 0x593af0 - 0x593ffe). No RNG.
//
// The engine passes the region the ramp is for in ecx (our `owner`), the strip
// corners and the region it ramps towards on the stack. The body:
//   1. w = to.X - from.X and h = to.Y - from.Y; both must reach 3 (0x593b13).
//   2. block = (from.X - 7, from.Y - 4) of (w + 10) x (h + 10); sub_594010 must
//      accept it, i.e. the block may only hold cells of `owner` or of `targetId`,
//      each carrying a placeholder tile.
//   3. five stamp loops: mark the cells with a region id and set their Level to
//      that region's Level - the first two with targetId / the target's Level,
//      the last three with owner->id / owner->level:
//        a) rows from.Y - 4        for h + 10, cols from.X - 7 for 7
//        b) rows to.Y + 1          for 7,      cols from.X - 7 for w + 10
//        c) row  from.Y,                       cols from.X .. from.X + 1
//        d) rows to.Y - 1 .. to.Y,             col  to.X
//        e) rows from.Y + 1 .. to.Y - 1,       cols from.X + 1 .. to.X - 1
//   4. two 2 x 2 slope-set pieces through sub_5A6C10 (PlaceWaterDetailTile): tile
//      slopeSetPiecesIndex_ + 7 at (from.X - 3, from.Y) and + 1 at
//      (to.X - 2, to.Y), both stamped with targetId at owner's Level.
//   5. three 4-wide ramp bands, each cell taking SlopeIndex, Level owner->level -
//      n - 1, its RampBase tile and Height 0 (no diamond gate - the vanilla calls
//      GetCellAt_MapCrd directly):
//        A) n = 0..3: (from.X - n, to.Y + n)                      Slope 8, tile + 7
//        B) n = 0..3: (from.X - n, from.Y + 3 + i), i < h + n - 3 Slope 1, tile + 0
//        C) n = 0..3: (to.X - 3 - j, to.Y + n),   j < w + n - 3   Slope 4, tile + 3
//   6. return 1 (0x593ffe).
int RandomMapGenerator::RampBuilder1(MapRegion* owner, CellStruct from, CellStruct to,
                                     int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return 0;

    const int w = to.X - from.X;                          // 0x593af6
    if (w < 3)
        return 0;
    const int h = to.Y - from.Y;
    if (h < 3)
        return 0;

    const int blockW = w + 10;                            // 0x593b1x
    const int blockH = h + 10;
    const int originX = from.X - 7;
    const int originY = from.Y - 4;
    if (!RampRectClear(owner, originX, originY, blockW, blockH, targetId))  // 0x593b4x
        return 0;

    MapRegion* target = FindRegionById(targetId);         // sub_5943E0
    const int targetLevel = (target != nullptr) ? target->level : 0;

    // a) the block's first band: h + 10 rows from originY, 7 columns from originX
    for (int y = originY; y < originY + blockH; ++y)
    {
        for (int x = originX; x < originX + 7; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;                                 // sub_5AC230
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);                     // 0x593cxx sub_58D070
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // b) rows to.Y + 1 .. to.Y + 7, columns originX .. originX + blockW - 1
    for (int y = to.Y + 1; y < to.Y + 8; ++y)
    {
        for (int x = originX; x < originX + blockW; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    const int ownerLevel = owner->level;                  // *(this + 16)

    // c) row from.Y, columns from.X .. from.X + 1
    for (int y = from.Y; y < from.Y + 1; ++y)
    {
        for (int x = from.X; x < from.X + 2; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);                    // 0x593exx
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // d) rows to.Y - 1 .. to.Y, column to.X
    for (int y = to.Y - 1; y < to.Y + 1; ++y)
    {
        for (int x = to.X; x < to.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // e) rows from.Y + 1 .. to.Y - 1, columns from.X + 1 .. to.X - 1
    for (int y = from.Y + 1; y < to.Y; ++y)
    {
        for (int x = from.X + 1; x < to.X; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 4. the two slope-set pieces (0x593f4x - 0x593f9x)
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 7,
                         PackCoords(CellStruct{ static_cast<int16_t>(from.X - 3),
                                                from.Y }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 1,
                         PackCoords(CellStruct{ static_cast<int16_t>(to.X - 2), to.Y }),
                         targetId, ownerLevel);

    // 5. band A - the diagonal run (0x593f9x)
    for (int n = 0; n < 4; ++n)
    {
        MapCell* cell = CellAt(static_cast<int16_t>(from.X - n),
                               static_cast<int16_t>(to.Y + n));
        cell->SlopeIndex = 8;
        cell->Level = ownerLevel - n - 1;
        cell->IsoTileTypeIndex = rampBaseIndex_ + 7;
        cell->Height = 0;
    }

    // band B - the downward run (0x593fcx)
    for (int n = 0; n < 4; ++n)
    {
        for (int i = 0; i < h + n - 3; ++i)
        {
            MapCell* cell = CellAt(static_cast<int16_t>(from.X - n),
                                   static_cast<int16_t>(from.Y + i + 3));
            cell->SlopeIndex = 1;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_;
            cell->Height = 0;
        }
    }

    // band C - the sideways run (0x593fex)
    for (int n = 0; n < 4; ++n)
    {
        for (int j = 0; j < w + n - 3; ++j)
        {
            MapCell* cell = CellAt(static_cast<int16_t>(to.X - j - 3),
                                   static_cast<int16_t>(to.Y + n));
            cell->SlopeIndex = 4;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_ + 3;
            cell->Height = 0;
        }
    }

    return 1;
}

// sub_593550 - the second ramp builder, the mirror of sub_593AF0 (593550.c,
// 0x593550 - 0x593ae1). No RNG.
//
// The engine passes the owner region in ecx, then the two strip corners - here
// `from` is the vanilla's a2 and `to` its p_pMapCoord_1, i.e. the strip runs the
// other way round: w = from.X - to.X and h = to.Y - from.Y, both at least 3.
//
//   1. block = (to.X - 7, to.Y - 7) of (w + 10) x (h + 10); the same sub_594010
//      coverage precondition as sub_593AF0.
//   2. five stamp loops, the first two with the target region, the last three
//      with the owner:
//        a) rows to.Y - 7        for 7,      cols to.X - 7 for w + 10
//        b) rows to.Y - 7        for h + 10, cols to.X - 7 for 7
//        c) rows from.Y .. from.Y + 1,       col  from.X
//        d) row  to.Y,                       cols to.X .. to.X + 1
//        e) rows from.Y + 1 .. to.Y - 1,     cols to.X + 1 .. from.X - 1
//      (note the mirroring: loops c/d are transposed relative to sub_593AF0.)
//   3. FOUR 2 x 2 slope-set pieces through sub_5A6C10 at
//        slopeSetPiecesIndex_ + 5 -> (from.X - 2, from.Y - 3)
//        slopeSetPiecesIndex_ + 6 -> (from.X - 1, from.Y - 1)
//        slopeSetPiecesIndex_ + 8 -> (to.X   - 3, to.Y   - 2)
//        slopeSetPiecesIndex_ + 9 -> (to.X   - 1, to.Y   - 1)
//      all stamped with targetId at owner's Level.
//   4. three 4-wide ramp bands, each at Level owner->level - n - 1 and Height 0:
//        A) n = 0..3: (to.X - n, from.Y - n)                      Slope 5, tile + 4
//        B) n = 0..3: (from.X - 3 - i, from.Y - n), i < w + n - 3 Slope 2, tile + 1
//        C) n = 0..3: (to.X - n, to.Y - 3 - j),     j < h + n - 3 Slope 1, tile + 0
int RandomMapGenerator::RampBuilder2(MapRegion* owner, CellStruct from, CellStruct to,
                                     int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return 0;

    const int w = from.X - to.X;                          // 0x59355x
    if (w < 3)
        return 0;
    const int h = to.Y - from.Y;
    if (h < 3)
        return 0;

    const int blockW = w + 10;
    const int blockH = h + 10;
    const int originX = to.X - 7;                         // 0x59358x
    const int originY = to.Y - 7;
    if (!RampRectClear(owner, originX, originY, blockW, blockH, targetId))  // 0x5935ax
        return 0;

    MapRegion* target = FindRegionById(targetId);         // sub_5943E0
    const int targetLevel = (target != nullptr) ? target->level : 0;
    const int ownerLevel = owner->level;


    // a) rows originY .. originY + 6, columns originX .. originX + blockW - 1
    for (int y = originY; y < originY + 7; ++y)
    {
        for (int x = originX; x < originX + blockW; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);                     // sub_58D070
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // b) rows originY .. originY + blockH - 1, columns originX .. originX + 6
    for (int y = originY; y < originY + blockH; ++y)
    {
        for (int x = originX; x < originX + 7; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // c) rows from.Y .. from.Y + 1, column from.X
    for (int y = from.Y; y < from.Y + 2; ++y)
    {
        for (int x = from.X; x < from.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // d) row to.Y, columns to.X .. to.X + 1
    for (int y = to.Y; y < to.Y + 1; ++y)
    {
        for (int x = to.X; x < to.X + 2; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // e) rows from.Y + 1 .. to.Y - 1, columns to.X + 1 .. from.X - 1
    for (int y = from.Y + 1; y < to.Y; ++y)
    {
        for (int x = to.X + 1; x < from.X; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 3. the four slope-set pieces (0x5938xx - 0x5939xx)
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 5,
                         PackCoords(CellStruct{ static_cast<int16_t>(from.X - 2),
                                                static_cast<int16_t>(from.Y - 3) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 6,
                         PackCoords(CellStruct{ static_cast<int16_t>(from.X - 1),
                                                static_cast<int16_t>(from.Y - 1) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 8,
                         PackCoords(CellStruct{ static_cast<int16_t>(to.X - 3),
                                                static_cast<int16_t>(to.Y - 2) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 9,
                         PackCoords(CellStruct{ static_cast<int16_t>(to.X - 1),
                                                static_cast<int16_t>(to.Y - 1) }),
                         targetId, ownerLevel);

    // 4. band A - the anti-diagonal run (0x593a0x)
    for (int n = 0; n < 4; ++n)
    {
        MapCell* cell = CellAt(static_cast<int16_t>(to.X - n),
                               static_cast<int16_t>(from.Y - n));
        cell->SlopeIndex = 5;
        cell->Level = ownerLevel - n - 1;
        cell->IsoTileTypeIndex = rampBaseIndex_ + 4;
        cell->Height = 0;
    }

    // band B - the upward run (0x593a5x)
    for (int n = 0; n < 4; ++n)
    {
        for (int i = 0; i < w + n - 3; ++i)
        {
            MapCell* cell = CellAt(static_cast<int16_t>(from.X - i - 3),
                                   static_cast<int16_t>(from.Y - n));
            cell->SlopeIndex = 2;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_ + 1;
            cell->Height = 0;
        }
    }

    // band C - the sideways run (0x593aax)
    for (int n = 0; n < 4; ++n)
    {
        for (int j = 0; j < h + n - 3; ++j)
        {
            MapCell* cell = CellAt(static_cast<int16_t>(to.X - n),
                                   static_cast<int16_t>(to.Y - j - 3));
            cell->SlopeIndex = 1;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_;
            cell->Height = 0;
        }
    }

    return 1;
}

// sub_593030 - the third ramp builder, shared by two cascade branches (593030.c,
// 0x593030 - 0x593546). No RNG.
//
// Same argument convention as sub_593550 (from = the vanilla's a2, to =
// p_pMapCoord_1) and the same "from - to" differences: w = from.X - to.X and
// h = from.Y - to.Y, both at least 3.
//
//   1. block = (to.X - 2, to.Y - 7) of (w + 10) x (h + 10), same sub_594010
//      coverage precondition.
//   2. five stamp loops:
//        a) rows to.Y - 7        for 7,      cols to.X - 2 for w + 10   -> target
//        b) rows to.Y - 7        for h + 10, cols from.X + 1 for 7      -> target
//        c) rows to.Y .. to.Y + 1,           col  to.X                  -> owner
//        d) row  from.Y,                     cols from.X - 1 .. from.X  -> owner
//        e) rows to.Y + 1 .. from.Y - 1,     cols to.X + 1 .. from.X - 1 -> owner
//      (loop b starts at from.X + 1, not at the block origin - it is the only
//      stamp loop in the four builders that does not walk the block.)
//   3. two 2 x 2 slope-set pieces through sub_5A6C10:
//        slopeSetPiecesIndex_ + 2 -> (from.X,     from.Y - 2)
//        slopeSetPiecesIndex_ + 4 -> (to.X,       to.Y   - 3)
//      both stamped with targetId at owner's Level.
//   4. three 4-wide ramp bands, each at Level owner->level - n - 1 and Height 0:
//        A) n = 0..3: (from.X + n, to.Y - n)                       Slope 6, tile + 5
//        B) n = 0..3: (from.X + n, from.Y - 3 - i), i < h + n - 3  Slope 3, tile + 2
//        C) n = 0..3: (to.X + 3 + j, to.Y - n),     j < w + n - 3  Slope 2, tile + 1
int RandomMapGenerator::RampBuilder3(MapRegion* owner, CellStruct from, CellStruct to,
                                     int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return 0;

    const int w = from.X - to.X;                          // 0x59303x
    if (w < 3)
        return 0;
    const int h = from.Y - to.Y;
    if (h < 3)
        return 0;

    const int blockW = w + 10;
    const int blockH = h + 10;
    const int originX = to.X - 2;                         // 0x59306x
    const int originY = to.Y - 7;
    if (!RampRectClear(owner, originX, originY, blockW, blockH, targetId))  // 0x59308x
        return 0;

    MapRegion* target = FindRegionById(targetId);         // sub_5943E0
    const int targetLevel = (target != nullptr) ? target->level : 0;
    const int ownerLevel = owner->level;

    // a) rows originY .. originY + 6, columns originX .. originX + blockW - 1
    for (int y = originY; y < originY + 7; ++y)
    {
        for (int x = originX; x < originX + blockW; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);                     // sub_58D070
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // b) rows originY .. originY + blockH - 1, columns from.X + 1 .. from.X + 7
    for (int y = originY; y < originY + blockH; ++y)
    {
        for (int x = from.X + 1; x < from.X + 8; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // c) rows to.Y .. to.Y + 1, column to.X
    for (int y = to.Y; y < to.Y + 2; ++y)
    {
        for (int x = to.X; x < to.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // d) row from.Y, columns from.X - 1 .. from.X
    for (int y = from.Y; y < from.Y + 1; ++y)
    {
        for (int x = from.X - 1; x < from.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // e) rows to.Y + 1 .. from.Y - 1, columns to.X + 1 .. from.X - 1
    for (int y = to.Y + 1; y < from.Y; ++y)
    {
        for (int x = to.X + 1; x < from.X; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 3. the two slope-set pieces (0x5937xx)
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 2,
                         PackCoords(CellStruct{ from.X,
                                                static_cast<int16_t>(from.Y - 2) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 4,
                         PackCoords(CellStruct{ to.X,
                                                static_cast<int16_t>(to.Y - 3) }),
                         targetId, ownerLevel);

    // 4. band A - the diagonal run (0x5937xx)
    for (int n = 0; n < 4; ++n)
    {
        MapCell* cell = CellAt(static_cast<int16_t>(from.X + n),
                               static_cast<int16_t>(to.Y - n));
        cell->SlopeIndex = 6;
        cell->Level = ownerLevel - n - 1;
        cell->IsoTileTypeIndex = rampBaseIndex_ + 5;
        cell->Height = 0;
    }

    // band B - the downward run (0x59384x)
    for (int n = 0; n < 4; ++n)
    {
        for (int i = 0; i < h + n - 3; ++i)
        {
            MapCell* cell = CellAt(static_cast<int16_t>(from.X + n),
                                   static_cast<int16_t>(from.Y - 3 - i));
            cell->SlopeIndex = 3;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_ + 2;
            cell->Height = 0;
        }
    }

    // band C - the sideways run (0x5938ax)
    for (int n = 0; n < 4; ++n)
    {
        for (int j = 0; j < w + n - 3; ++j)
        {
            MapCell* cell = CellAt(static_cast<int16_t>(to.X + 3 + j),
                                   static_cast<int16_t>(to.Y - n));
            cell->SlopeIndex = 2;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_ + 1;
            cell->Height = 0;
        }
    }

    return 1;
}
// sub_592440 - the fourth ramp builder. It serves the fifth cascade branch (mask
// 0x70 == 0 && 0x88 set) and the tail sweep's last attempt (592440.c,
// 0x592440 - 0x592ae3).
//
// Arguments as sub_593AF0 (owner in ecx, `from` = a2-ish first corner, `to` =
// second corner, then the target id). This one derives the block from the
// min/max of the two X and works with the *horizontal distance*:
//
//   minX = min(from.X, to.X), maxX = max(from.X, to.X)
//   block = (minX - 7, from.Y - 4) of (maxX - minX + 11) x (to.Y - from.Y + 9)
//   dx = to.X - from.X, dy = to.Y - from.Y, adx = |dx|; the whole routine bails
//   out when adx > dy - 5 (0x59244x), and half = (dy + 1) / 2 + 2.
//
//   1. rows from.Y - 4 .. half + from.Y - 3, cols minX - 7 .. from.X - 1  -> target
//   2. rows to.Y - half + 3 .. to.Y + 4,     cols minX - 7 .. to.X - 1    -> target
//   3. rows from.Y .. half + from.Y - 2,     cols from.X .. maxX + 2      -> owner
//   4. rows to.Y - half + 3 .. to.Y,         cols to.X .. maxX + 2        -> owner
//   5. three 2 x 2 pieces (slopeSetPiecesIndex_ + 7 / + 8 / + 9) at
//      (from.X - 3, from.Y), (to.X - 3, to.Y - 2) and (to.X - 1, to.Y - 1),
//      target id / owner's Level.
//   6. when dx != 0: ONE RNG draw of 0/1 (0x5928xx). side = (dx > 0). Two small
//      byte tables drive a 5-cell strip per column, |dx| columns:
//          levelByte = kLevelOffsets[i]  -> cell Level = owner->level + byte - 4
//          slopeByte = kSlopeOffsets[side][i] -> SlopeIndex and
//                       IsoTileTypeIndex = slopeByte + rampBase - 1
//      with the coordinates (column counts 0 .. |dx| - 1, i is the table index)
//          x = (side ? column : -column) + (side + from.X - i)
//          y = column + from.Y + 3 + ((draw == 1) ? 0 : to.Y - adx - from.Y - 5)
//      and the slope tables
//          kSlopeOffsets[side][5] = {9, 13, 13, 13, 5}   (0x82B004 + 4i)
//                                   {12, 16, 16, 16, 8}  (0x82B018 + 4i)
//      (the vanilla adds the packed CellStructs, which is safe here because the
//       values never carry across the 16-bit halves.)
//   7. rows loop for (to.Y - from.Y - adx - 5) rows x 4 cells: the work mark is
//      set to targetId directly (0x5929xx `*(work + ... ) = a4`) and the cells get
//      SlopeIndex 1, Level owner->level - n - 1, tile rampBase, Height 0.
//   8. return 1.
int RandomMapGenerator::RampBuilder4(MapRegion* owner, CellStruct from, CellStruct to,
                                     int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return 0;

    const int minX = (from.X <= to.X) ? from.X : to.X;    // 0x59244x
    const int maxX = (from.X >= to.X) ? from.X : to.X;
    const int blockX = minX - 7;
    const int blockY = from.Y - 4;
    const int blockW = maxX - minX + 11;
    const int blockH = to.Y - from.Y + 9;
    if (!RampRectClear(owner, blockX, blockY, blockW, blockH, targetId))  // 0x5924xx
        return 0;

    const int dx = to.X - from.X;                         // Y_12
    const int dy = to.Y - from.Y;                         // v12
    const int adx = (dx < 0) ? -dx : dx;                  // v83
    if (adx > dy - 5)                                     // 0x5924xx
        return 0;

    MapRegion* target = FindRegionById(targetId);         // sub_5943E0
    const int targetLevel = (target != nullptr) ? target->level : 0;
    const int ownerLevel = owner->level;
    const int half = (dy + 1) / 2 + 2;                    // v17

    // 1. rows blockY .. half + from.Y - 3 (the bound is v17 + blockY + 2),
    //    columns blockX .. from.X - 1
    //
    // PORT FIX (ramp-into-cliff, NW-facing Builder4): vanilla sub_592440 ends
    // this target-level stamp just before loop 2 starts, but for an EVEN dy the two
    // bounds leave exactly ONE row uncovered (B - A = dy + 1 - 2*((dy+1)/2),
    // which is 1 for even dy and 0 for odd dy). That row lands on a middle row
    // of the tail slope strip: the tail ramp tiles it from x = from.X on,
    // while its west side (x < from.X) keeps the old owner Level (L8). The
    // westernmost leftover L8 cell then sits directly west of the L4 ramp-foot
    // cell - a +4 edge PlaceCliffs turns into a wall stamped on top of the
    // ramp (seen at (72,72) next to ramp foot (73,72), dy=8). Run this stamp
    // straight up to loop 2's start row. For odd dy the new bound equals the old
    // bound (no extra cell); for even dy it flattens only that one gap row.
    // Columns stay x < from.X, so the owner shoulder (x >= from.X, loops 3/4)
    // and the tail strip itself are untouched.
    for (int y = blockY; y < to.Y - half + 3; ++y)
    {
        for (int x = blockX; x < from.X; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 2. rows to.Y - half + 3 .. to.Y + 4, columns blockX .. to.X - 1
    for (int y = to.Y - half + 3; y < to.Y + 5; ++y)
    {
        for (int x = blockX; x < to.X; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 3. rows from.Y .. half + from.Y - 2, columns from.X .. maxX + 2
    for (int y = from.Y; y < half + from.Y - 1; ++y)
    {
        for (int x = from.X; x < maxX + 3; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 4. rows to.Y - half + 3 .. to.Y, columns to.X .. maxX + 2
    for (int y = to.Y - half + 3; y < to.Y + 1; ++y)
    {
        for (int x = to.X; x < maxX + 3; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 5. the three slope-set pieces (0x5926xx)
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 7,
                         PackCoords(CellStruct{ static_cast<int16_t>(from.X - 3),
                                                from.Y }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 8,
                         PackCoords(CellStruct{ static_cast<int16_t>(to.X - 3),
                                                static_cast<int16_t>(to.Y - 2) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 9,
                         PackCoords(CellStruct{ static_cast<int16_t>(to.X - 1),
                                                static_cast<int16_t>(to.Y - 1) }),
                         targetId, ownerLevel);

    // 6. the table-driven strip (0x5928xx).
    //
    // Vanilla sub_592440 zeroes v73 BEFORE `if (dx)` and runs the step-7 tail
    // strip OUTSIDE that gate (592440.c lines 276 and 349-383): on a straight
    // vertical strip (the NW-facing case, dx == 0) the three big pieces are
    // still placed, and the tail is what fills the corridor between them. An
    // earlier port nested the tail inside `if (dx != 0)`, so dx == 0 left the
    // middle rows untouched at the owner's high Level and the two high bodies
    // stayed joined by a one-row ridge, later stamped as a cliff straight
    // through the ramp. Keep the biases out here, zero-initialised.
    int tailBiasLow = 0;                                 // LOWORD(v73)
    int tailBiasHigh = 0;                                // HIWORD(v73)
    if (dx != 0)
    {
        const int side = (dx > 0) ? 1 : 0;                // Y_9 / Y_1
        int draw;
        do
        {
            draw = DrawZeroOrOne();                       // [RNG] 0/1
        }
        while (draw > 1);

        // Two different biases drive the two loops:
        //   - the strip loop adds Y_10 = pack(side, draw == 1 ? 0
        //                                            : to.Y - adx - from.Y - 5)
        //   - the tail loop adds v73 = (draw == 1) ? pack(dx, adx) : 0
        const int stripBiasLow = side;
        const int stripBiasHigh = (draw == 1) ? 0 : (to.Y - adx - from.Y - 5);
        if (draw == 1)
        {
            tailBiasLow = static_cast<int16_t>(dx);
            tailBiasHigh = adx;
        }

        static const int kLevelOffsets[5] = { 3, 2, 1, 0, 0 };        // 0x82AFF0 + 4i
        static const int kSlopeOffsets[2][5] = {                      // 0x82B004 / 0x82B018
            { 9, 13, 13, 13, 5 },
            { 12, 16, 16, 16, 8 }
        };

        if (adx > 0)
        {
            int column = 0;                               // pMapCoord_.x, the ++ counter
            int mirror = 0;                               // X_1, decremented
            do
            {
                const int columnPart = (side != 0) ? column : mirror;    // X_7
                for (int i = 0; i < 5; ++i)
                {
                    // cell.X = columnPart + (int16)(stripBiasLow + from.X - i)
                    // cell.Y = column + from.Y + 3 + stripBiasHigh
                    const int cx = static_cast<int16_t>(
                        columnPart + static_cast<int16_t>(stripBiasLow + from.X - i));
                    const int cy = static_cast<int16_t>(
                        column + from.Y + 3 + stripBiasHigh);
                    MapCell* cell = CellAt(static_cast<int16_t>(cx),
                                           static_cast<int16_t>(cy));
                    cell->Level = ownerLevel + kLevelOffsets[i] - 4;   // 0x59297x
                    const int slopeByte = kSlopeOffsets[side][i];
                    cell->SlopeIndex = slopeByte;                  // 0x592980
                    cell->Height = 0;                              // 0x592999
                    cell->IsoTileTypeIndex = slopeByte + rampBaseIndex_ - 1;
                }
                --mirror;
            }
            while (++column < adx);
        }
    }

    // 7. the tail strip (0x5929Dx - 0x592acx), 4 cells per row.
    // UNCONDITIONAL in vanilla: dx == 0 simply uses the zero biases above.
    const int tailRows = to.Y - from.Y - adx - 5;
    for (int row = 0; row < tailRows; ++row)
    {
        for (int n = 0; n < 4; ++n)
        {
            const int16_t cx = static_cast<int16_t>(tailBiasLow + from.X - n);
            const int16_t cy = static_cast<int16_t>(tailBiasHigh + from.Y + 3 + row);
            SetWorkMark(CellStruct{ cx, cy }, targetId);   // 0x5929xx (the mark)
            MapCell* cell = CellAt(cx, cy);
            cell->SlopeIndex = 1;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_;
            cell->Height = 0;
        }
    }

    return 1;
}
// sub_591D80 - the fifth ramp builder, the vertical mirror of sub_592440
// (591D80.c, 0x591d80 - 0x59243b). Serves the seventh cascade branch (mask
// 0xC1 == 0 && 0x22 set). No RNG apart from the one strip draw.
//
//   minY = min(from.Y, to.Y), maxY = max(from.Y, to.Y)
//   block = (to.X - 4, minY - 7) of (from.X - to.X + 8) x (maxY - minY + 11)
//   dx = from.X - to.X, dy = to.Y - from.Y, ady = |dy|; the routine bails out
//   when ady > dx - 5, and v17 = (dx + 1) / 2 + 3.
//
//   1. rows minY - 7 .. to.Y - 1,  cols to.X - 4 .. to.X + v17 - 3   -> target
//   2. rows minY - 7 .. from.Y - 1, cols from.X - v17 + 2 .. from.X + 4 -> target
//   3. rows to.Y .. maxY + 1,      cols to.X .. to.X + v17 - 3        -> owner
//   4. rows from.Y .. maxY + 1,    cols from.X - v17 + 2 .. from.X + 1 -> owner
//   5. three 2 x 2 pieces: slopeSetPiecesIndex_ + 4 -> (to.X, to.Y - 3),
//      + 5 -> (from.X - 2, from.Y - 3), + 6 -> (from.X - 1, from.Y - 1)
//   6. when dy != 0: one 0/1 draw, side = (dy > 0), then a 5-cell strip per
//      column for ady columns, driven by the byte tables
//        kLevelOffsets           = {3, 2, 1, 0, 0}     (0x82AFB4 + 4i)
//        kSlopeOffsets[side][5]  = {10, 14, 14, 14, 6} (0x82AFC8 + 4i)
//                                  {9, 13, 13, 13, 5}  (0x82AFDC + 4i)
//      with the low-word bias v70 = (draw == 1) ? 0 : from.X - ady - to.X - 5 and
//      the column offset Y_1 = side ? -column : column (column counts 0, 1, 2, ..).
//   7. tail strip: (from.X - to.X - ady - 5) rows x 4 cells, the work mark set to
//      targetId, cells at SlopeIndex 2, Level owner->level - n - 1, tile
//      rampBase + 1, Height 0, shifted by the packed bias v68 = (draw == 1)
//      ? pack(ady, -dy) : 0.
//   8. return 1.
int RandomMapGenerator::RampBuilder5(MapRegion* owner, CellStruct from, CellStruct to,
                                     int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return 0;

    const int minY = (from.Y <= to.Y) ? from.Y : to.Y;    // 0x591d9x
    const int maxY = (from.Y >= to.Y) ? from.Y : to.Y;
    const int blockX = to.X - 4;
    const int blockY = minY - 7;
    const int blockW = from.X - to.X + 8;
    const int blockH = maxY - minY + 11;
    if (!RampRectClear(owner, blockX, blockY, blockW, blockH, targetId))  // 0x591dxx
        return 0;

    const int dy = to.Y - from.Y;
    const int ady = (dy < 0) ? -dy : dy;                  // p_pMapCoordb
    const int dx = from.X - to.X;                         // v13
    if (ady > dx - 5)                                     // 0x591dxx
        return 0;

    MapRegion* target = FindRegionById(targetId);         // sub_5943E0
    const int targetLevel = (target != nullptr) ? target->level : 0;
    const int ownerLevel = owner->level;
    const int v17 = (dx + 1) / 2 + 3;                     // 0x591dxx

    // 1. rows minY - 7 .. to.Y - 1, columns blockX .. blockX + v17 + 1
    for (int y = blockY; y < to.Y; ++y)
    {
        for (int x = blockX; x < blockX + v17 + 2; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 2. rows minY - 7 .. from.Y - 1, columns from.X - v17 + 2 .. from.X + 4
    for (int y = blockY; y < from.Y; ++y)
    {
        for (int x = from.X - v17 + 2; x < from.X + 5; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 3. rows to.Y .. maxY + 1, columns to.X .. to.X + v17 - 3
    for (int y = to.Y; y < maxY + 2; ++y)
    {
        for (int x = to.X; x < to.X + v17 - 2; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 4. rows from.Y .. maxY + 1, columns from.X - v17 + 2 .. from.X
    for (int y = from.Y; y < maxY + 2; ++y)
    {
        for (int x = from.X - v17 + 2; x < from.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 5. the three slope-set pieces (0x5920xx)
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 4,
                         PackCoords(CellStruct{ to.X,
                                                static_cast<int16_t>(to.Y - 3) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 5,
                         PackCoords(CellStruct{ static_cast<int16_t>(from.X - 2),
                                                static_cast<int16_t>(from.Y - 3) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 6,
                         PackCoords(CellStruct{ static_cast<int16_t>(from.X - 1),
                                                static_cast<int16_t>(from.Y - 1) }),
                         targetId, ownerLevel);

    // 6. the table-driven strip (0x5921xx - 0x5923xx)
    int tailBiasLow = 0;
    int tailBiasHigh = 0;
    if (dy != 0)
    {
        const int side = (dy > 0) ? 1 : 0;                // pMapCoord___7
        int draw;
        do
        {
            draw = DrawZeroOrOne();                       // [RNG] 0/1
        }
        while (draw > 1);

        const int stripBias = (draw == 1) ? 0 : (from.X - ady - to.X - 5);   // v70
        tailBiasLow = (draw == 1) ? ady : 0;              // LOWORD(v68)
        tailBiasHigh = (draw == 1) ? -dy : 0;             // HIWORD(v68)

        static const int kLevelOffsets[5] = { 3, 2, 1, 0, 0 };        // 0x82AFB4 + 4i
        static const int kSlopeOffsets[2][5] = {                      // 0x82AFC8 / 0x82AFDC
            { 10, 14, 14, 14, 6 },
            { 9, 13, 13, 13, 5 }
        };

        if (ady > 0)
        {
            const int belowZero = (dy < 0) ? 1 : 0;       // v71
            int counter = 0;                              // pMapCoord_
            int previous = 0;                             // pMapCoord__2
            int mirror = 0;                               // Y, the column offset
            do
            {
                const int column = (side != 0) ? mirror : previous;      // Y_1
                for (int i = 0; i < 5; ++i)
                {
                    const int cx = static_cast<int16_t>(
                        previous + to.X + 3 + stripBias);               // v74 + v70
                    const int cy = static_cast<int16_t>(
                        column + belowZero + to.Y - i);                 // Y_1 + WORD1
                    MapCell* cell = CellAt(static_cast<int16_t>(cx),
                                           static_cast<int16_t>(cy));
                    cell->Level = ownerLevel + kLevelOffsets[i] - 4;    // 0x5922xx
                    const int slopeByte = kSlopeOffsets[side][i];
                    cell->SlopeIndex = slopeByte;                       // 0x5922xx
                    cell->Height = 0;
                    cell->IsoTileTypeIndex = slopeByte + rampBaseIndex_ - 1;
                }
                ++counter;                                            // 0x5923xx
                previous = counter;
                --mirror;
            }
            while (counter < ady);
        }
    }

    // 7. the tail strip (0x5923xx), 4 cells per row
    const int tailRows = from.X - to.X - ady - 5;
    for (int row = 0; row < tailRows; ++row)
    {
        for (int n = 0; n < 4; ++n)
        {
            const int16_t cx = static_cast<int16_t>(tailBiasLow + row + to.X + 3);
            const int16_t cy = static_cast<int16_t>(tailBiasHigh + to.Y - n);
            SetWorkMark(CellStruct{ cx, cy }, targetId);      // 0x5923xx (the mark)
            MapCell* cell = CellAt(cx, cy);
            cell->SlopeIndex = 2;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_ + 1;
            cell->Height = 0;
        }
    }

    return 1;
}
// sub_591740 - the sixth ramp builder (591740.c, 0x591740 - 0x591d7e). Serves the
// sixth cascade branch (mask 0x07 == 0 && 0x88 set) and the tail sweep's second
// attempt.
//
//   minX = min(from.X, to.X), maxX = max(from.X, to.X)
//   block = (minX - 3, to.Y - 4) of (maxX - minX + 11) x (from.Y - to.Y + 9)
//   dx = to.X - from.X, dy = from.Y - to.Y, adx = |dx|; bails out when
//   adx > dy - 5; q = (dy + 1) / 2.
//
//   1. rows from.Y - q .. from.Y + 4, cols from.X + 1 .. from.X + 7   -> target
//   2. rows to.Y - 4 .. to.Y + q - 1, cols to.X + 1 .. to.X + 7       -> target
//   3. rows from.Y - q .. from.Y,     cols minX - 1 .. from.X          -> owner
//   4. rows to.Y .. to.Y + q - 1,     cols minX - 1 .. to.X            -> owner
//   5. two 2 x 2 pieces: slopeSetPiecesIndex_ + 2 -> (from.X, from.Y - 2) and
//      + 3 -> (to.X, to.Y), target id / owner's Level.
//   6. when dx != 0: one 0/1 draw, side = (dx > 0), sign = (dx >= 0) ? 0 : -1;
//      a 5-cell strip per column for adx columns driven by
//        kLevelOffsets          = {3, 2, 1, 0, 0}     (0x82AF78 + 4i)
//        kSlopeOffsets[side][5] = {10, 14, 14, 14, 6} (0x82AF8C + 4i)
//                                 {11, 15, 15, 15, 7} (0x82AFA0 + 4i)
//      with the packed bias v75 = (draw == 1) ? pack(sign, sign)
//                                             : pack(sign, -(from.Y - adx - to.Y - 5))
//      and the column offset (side ? counter : mirror).
//   7. tail strip: (from.Y - to.Y - adx - 5) rows x 4 cells, the work mark set to
//      targetId, cells at SlopeIndex 3, Level owner->level - n - 1, tile
//      rampBase + 2, Height 0, shifted by v76 = (draw == 1) ? pack(dx, -adx) : 0.
//   8. return 1.
int RandomMapGenerator::RampBuilder6(MapRegion* owner, CellStruct from, CellStruct to,
                                     int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return 0;

    const int minX = (from.X <= to.X) ? from.X : to.X;    // 0x59175x
    const int maxX = (from.X >= to.X) ? from.X : to.X;
    const int blockX = minX - 3;
    const int blockY = to.Y - 4;
    const int blockW = maxX - minX + 11;
    const int blockH = from.Y - to.Y + 9;
    if (!RampRectClear(owner, blockX, blockY, blockW, blockH, targetId))  // 0x5917xx
        return 0;

    const int dy = from.Y - to.Y;                         // v13
    const int dx2 = to.X - from.X;                        // v72
    const int adx = (dx2 < 0) ? -dx2 : dx2;               // v86
    if (adx > dy - 5)                                     // 0x5917xx
        return 0;

    MapRegion* target = FindRegionById(targetId);         // sub_5943E0
    const int targetLevel = (target != nullptr) ? target->level : 0;
    const int ownerLevel = owner->level;
    const int q = (dy + 1) / 2;                           // (v13 + 1) / 2

    // 1. rows from.Y - q .. from.Y + 4, columns from.X + 1 .. from.X + 7
    for (int y = from.Y - q; y < from.Y + 5; ++y)
    {
        for (int x = from.X + 1; x < from.X + 8; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 2. rows to.Y - 4 .. to.Y + q - 1, columns to.X + 1 .. to.X + 7
    for (int y = to.Y - 4; y < to.Y + q; ++y)
    {
        for (int x = to.X + 1; x < to.X + 8; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 3. rows from.Y - q .. from.Y, columns blockX + 2 .. from.X
    for (int y = from.Y - q; y < from.Y + 1; ++y)
    {
        for (int x = blockX + 2; x < from.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 4. rows to.Y .. to.Y + q - 1, columns blockX + 2 .. to.X
    for (int y = to.Y; y < to.Y + q; ++y)
    {
        for (int x = blockX + 2; x < to.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 5. the two slope-set pieces (0x591axx)
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 2,
                         PackCoords(CellStruct{ from.X,
                                                static_cast<int16_t>(from.Y - 2) }),
                         targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 3,
                         PackCoords(to), targetId, ownerLevel);

    // 6. the table-driven strip (0x591bxx - 0x591cxx).
    // Same vanilla gate as sub_592440: sub_591740 zeroes v76 before `if (dx)`
    // (591740.c line 276) and its step-7 tail strip at lines 351-386 sits
    // OUTSIDE that gate. The earlier port nested the tail here, so a straight
    // vertical strip (dx2 == 0) kept its middle rows at the high Level.
    int tailLow = 0;                                     // LOWORD(v76)
    int tailHigh = 0;                                    // HIWORD(v76)
    if (dx2 != 0)
    {
        const int side = (dx2 > 0) ? 1 : 0;               // Y_9
        int draw;
        do
        {
            draw = DrawZeroOrOne();                       // [RNG] 0/1
        }
        while (draw > 1);

        const int sign = (dx2 >= 0) ? 0 : -1;             // LOWORD(v75)
        const int v75Low = sign;
        const int v75High = (draw == 1) ? sign : -(from.Y - adx - to.Y - 5);
        if (draw == 1)
        {
            tailLow = dx2;                                // LOWORD(v76)
            tailHigh = -adx;                              // HIWORD(v76)
        }

        static const int kLevelOffsets[5] = { 3, 2, 1, 0, 0 };        // 0x82AF78 + 4i
        static const int kSlopeOffsets[2][5] = {                      // 0x82AF8C / 0x82AFA0
            { 10, 14, 14, 14, 6 },
            { 11, 15, 15, 15, 7 }
        };

        if (adx > 0)
        {
            int counter = 0;                              // pMapCoord_
            int mirror = 0;                               // Y
            do
            {
                const int columnPart = (side != 0) ? counter : mirror;   // X_6
                for (int i = 0; i < 5; ++i)
                {
                    const int cx = static_cast<int16_t>(
                        columnPart + static_cast<int16_t>(v75Low + i + from.X));
                    const int cy = static_cast<int16_t>(
                        -counter + from.Y - 3 + v75High);
                    MapCell* cell = CellAt(static_cast<int16_t>(cx),
                                           static_cast<int16_t>(cy));
                    cell->Level = ownerLevel + kLevelOffsets[i] - 4;    // 0x591cxx
                    const int slopeByte = kSlopeOffsets[side][i];
                    cell->SlopeIndex = slopeByte;
                    cell->Height = 0;
                    cell->IsoTileTypeIndex = slopeByte + rampBaseIndex_ - 1;
                }
                ++counter;                                    // 0x591dxx
                --mirror;
            }
            while (counter < adx);
        }
    }

    // 7. the tail strip (0x591dxx), 4 cells per row.
    // UNCONDITIONAL in vanilla: dx2 == 0 uses the zero biases above.
    const int tailRows = from.Y - to.Y - adx - 5;
    for (int row = 0; row < tailRows; ++row)
    {
        for (int n = 0; n < 4; ++n)
        {
            const int16_t cx = static_cast<int16_t>(n + from.X + tailLow);
            const int16_t cy = static_cast<int16_t>(-row + from.Y - 3 + tailHigh);
            SetWorkMark(CellStruct{ cx, cy }, targetId);   // 0x591dxx (the mark)
            MapCell* cell = CellAt(cx, cy);
            cell->SlopeIndex = 3;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_ + 2;
            cell->Height = 0;
        }
    }

    return 1;
}
// sub_5910F0 - the seventh ramp builder (5910F0.c, 0x5910f0 - 0x59173f). Serves
// the eighth cascade branch (mask 0x1C == 0 && 0x22 set) and the first attempt of
// the ratio > 0.5 tail sweep. It is the transposed twin of sub_591740 (builder
// 6): the 5-cell strip runs in Y while the columns advance in X.
//
//   minY = min(from.Y, to.Y), maxY = max(from.Y, to.Y)
//   block = (from.X - 5, minY - 3) of (to.X - from.X + 9) x (maxY - minY + 11)
//   dy = to.Y - from.Y, dx = to.X - from.X, ady = |dy|; the routine bails out
//   when ady > dx - 5, and X_12 = (dx + 1) / 2 + 3.
//
//   1. rows from.Y + 1 .. maxY + 9, cols blockX           .. blockX + X_12 + 1 -> target
//   2. rows to.Y + 1   .. maxY + 9, cols to.X - (dx+1)/2  .. to.X + 4         -> target
//   3. rows blockY + 2 .. from.Y,   cols blockX + 4       .. blockX + X_12 + 1 -> owner
//   4. rows blockY + 2 .. to.Y,     cols to.X - X_12 + 2  .. to.X              -> owner
//   5. two 2 x 2 pieces through sub_5A6C10: slopeSetPiecesIndex_ + 0 -> from and
//      + 1 -> (to.X - 2, to.Y), target id / owner's Level.
//   6. when dy != 0: one 0/1 draw, side = (dy > 0), sign = (dy >= 0) ? 0 : -1;
//      a 5-cell vertical strip per column for ady columns, driven by
//        kLevelOffsets           = {3, 2, 1, 0, 0}       (0x82AF3C + 4i)
//        kSlopeOffsets[side][5]  = {11, 15, 15, 15, 7}   (0x82AF50 + 4i)
//                                  {12, 16, 16, 16, 8}   (0x82AF64 + 4i)
//      with the X/Y bias pair v43 = pack((draw == 1) ? 0 : to.X - ady - from.X - 5,
//      sign) and the column offset (side ? counter : -counter).
//   7. tail strip: (to.X - from.X - ady - 5) rows x 4 cells, the work mark set to
//      targetId, cells at SlopeIndex 4, Level owner->level - n - 1, tile
//      rampBase + 3, Height 0, shifted by X_10 = (draw == 1) ? pack(ady, dy) : 0.
//   8. return 1.
int RandomMapGenerator::RampBuilder7(MapRegion* owner, CellStruct from, CellStruct to,
                                     int targetId)
{
    if (owner == nullptr || workCells_ == nullptr)
        return 0;

    const int minY = (from.Y <= to.Y) ? from.Y : to.Y;    // 0x59111x
    const int maxY = (from.Y >= to.Y) ? from.Y : to.Y;
    const int blockX = from.X - 5;
    const int blockY = minY - 3;
    const int blockW = to.X - from.X + 9;
    const int blockH = maxY - minY + 11;
    if (!RampRectClear(owner, blockX, blockY, blockW, blockH, targetId))  // 0x5911xx
        return 0;

    const int dy = to.Y - from.Y;                         // pMapCoord__1
    const int ady = (dy < 0) ? -dy : dy;                  // v78
    const int dx = to.X - from.X;                         // v13
    if (ady > dx - 5)                                     // 0x5914xx
        return 0;

    MapRegion* target = FindRegionById(targetId);         // sub_5943E0
    const int targetLevel = (target != nullptr) ? target->level : 0;
    const int ownerLevel = owner->level;
    const int halfSlot = (dx + 1) / 2 + 3;                // X_12

    // 1. rows from.Y + 1 .. maxY + 9, columns blockX .. blockX + X_12 + 1
    for (int y = from.Y + 1; y < maxY + 10; ++y)
    {
        for (int x = blockX; x < blockX + halfSlot + 2; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);                     // sub_58D070
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 2. rows to.Y + 1 .. maxY + 9, columns to.X - (dx + 1) / 2 .. to.X + 4
    for (int y = to.Y + 1; y < maxY + 10; ++y)
    {
        for (int x = to.X - (dx + 1) / 2; x < to.X + 5; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, targetId);
            CellAt(c.X, c.Y)->Level = targetLevel;
        }
    }

    // 3. rows blockY + 2 .. from.Y, columns blockX + 4 .. blockX + X_12 + 1
    for (int y = blockY + 2; y < from.Y + 1; ++y)
    {
        for (int x = blockX + 4; x < blockX + halfSlot + 2; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 4. rows blockY + 2 .. to.Y, columns to.X - X_12 + 2 .. to.X
    for (int y = blockY + 2; y < to.Y + 1; ++y)
    {
        for (int x = to.X - halfSlot + 2; x < to.X + 1; ++x)
        {
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const CellStruct c{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            SetWorkMark(c, owner->id);
            CellAt(c.X, c.Y)->Level = ownerLevel;
        }
    }

    // 5. the two slope-set pieces (0x5914xx)
    PlaceWaterDetailTile(slopeSetPiecesIndex_,
                         PackCoords(from), targetId, ownerLevel);
    PlaceWaterDetailTile(slopeSetPiecesIndex_ + 1,
                         PackCoords(CellStruct{ static_cast<int16_t>(to.X - 2), to.Y }),
                         targetId, ownerLevel);

    // 6. the table-driven strip (0x5915xx) and 7. the tail strip (0x5916xx)
    int tailBiasLow = 0;                                  // LOWORD(X_10)
    int tailBiasHigh = 0;                                 // HIWORD(X_10)
    if (dy != 0)
    {
        const int side = (dy > 0) ? 1 : 0;                // X_11
        int draw;
        do
        {
            draw = DrawZeroOrOne();                       // [RNG] 0/1
        }
        while (draw > 1);

        const int stripBiasX = (draw == 1) ? 0 : (to.X - ady - from.X - 5);  // v66
        const int stripBiasY = (dy >= 0) ? 0 : -1;                           // v67
        if (draw == 1)
        {
            tailBiasLow = ady;
            tailBiasHigh = static_cast<int16_t>(dy);
        }

        static const int kLevelOffsets[5] = { 3, 2, 1, 0, 0 };        // 0x82AF3C + 4i
        static const int kSlopeOffsets[2][5] = {                      // 0x82AF50 / 0x82AF64
            { 11, 15, 15, 15, 7 },
            { 12, 16, 16, 16, 8 }
        };

        if (ady > 0)
        {
            int counter = 0;                              // pMapCoord_
            int mirror = 0;                               // Y, the column offset
            do
            {
                const int column = (side != 0) ? counter : mirror;   // Y_1
                for (int i = 0; i < 5; ++i)
                {
                    const int cx = static_cast<int16_t>(
                        counter + from.X + 3 + stripBiasX);
                    const int cy = static_cast<int16_t>(
                        column + stripBiasY + i + from.Y);
                    MapCell* cell = CellAt(static_cast<int16_t>(cx),
                                           static_cast<int16_t>(cy));
                    cell->Level = ownerLevel + kLevelOffsets[i] - 4;   // 0x5915xx
                    const int slopeByte = kSlopeOffsets[side][i];
                    cell->SlopeIndex = slopeByte;
                    cell->Height = 0;
                    cell->IsoTileTypeIndex = slopeByte + rampBaseIndex_ - 1;
                }
                ++counter;
                --mirror;
            }
            while (counter < ady);
        }
    }

    // 7. the tail strip (0x5916xx), 4 cells per row
    const int tailRows = to.X - from.X - ady - 5;
    for (int row = 0; row < tailRows; ++row)
    {
        for (int n = 0; n < 4; ++n)
        {
            const int16_t cx = static_cast<int16_t>(row + from.X + 3 + tailBiasLow);
            const int16_t cy = static_cast<int16_t>(n + from.Y + tailBiasHigh);
            SetWorkMark(CellStruct{ cx, cy }, targetId);   // 0x5916xx (the mark)
            MapCell* cell = CellAt(cx, cy);
            cell->SlopeIndex = 4;
            cell->Level = ownerLevel - n - 1;
            cell->IsoTileTypeIndex = rampBaseIndex_ + 3;
            cell->Height = 0;
        }
    }

    return 1;
}

// ---------------------------------------------------------------------------
// sub_5A7250 - "the rectangle carries nothing but pave / placeholder cells"
// (0x5a7250 - 0x5a73f2). The lateral-corridor builder (sub_58F2C0) probes its
// corridors with 3x1 / 1x3 / 3x3 rectangles through this test.
//
//   if any of the four corners of {x, y, w, h} is outside the diamond
//   (dword_ABED04 / dword_ABED08 - the window CellExists tests) -> false
//   for every cell of the w x h rectangle:
//       PavedRoads tile (sub_4866D0)    -> false unless allowPavedRoads
//       PavedRoadEnds tile (sub_4866F0) -> false unless allowPavedRoadEnds
//       otherwise: placeholder (0 / 0xFFFF) or MiscPaveTile (sub_486650) or
//                  PaveTile (sub_486670) must hold, else false
//   true
// ---------------------------------------------------------------------------
bool RandomMapGenerator::TileRectClear(int x, int y, int w, int h,
                                       bool allowPavedRoads,
                                       bool allowPavedRoadEnds)
{
    if (workCells_ == nullptr)
        return false;

    if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y))
        || !CellExists(static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y))
        || !CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y + h - 1))
        || !CellExists(static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + h - 1)))
        return false;

    for (int row = y; row < y + h; ++row)                  // 0x5a725x
    {
        for (int col = x; col < x + w; ++col)
        {
            const MapCell* cell = CellAt(col, row);
            if (IsPavedRoadTile(cell))                     // 0x5a72xx sub_4866D0
            {
                if (!allowPavedRoads)
                    return false;
            }
            else if (IsPavedRoadEndTile(cell))             // 0x5a72xx sub_4866F0
            {
                if (!allowPavedRoadEnds)
                    return false;
            }
            else if (!IsPlaceholderTile(cell)              // sub_486380
                     && !IsMiscPaveTile(cell)              // sub_486650
                     && !IsPaveTile(cell))                 // sub_486670
            {
                return false;
            }
        }
    }
    return true;
}

// sub_5A7440 - the same test plus a uniform Level (0x5a7440 - 0x5a743f).
//
//   same corner gate on {x, y, w, h}
//   level = the Level of the first cell (x, y)
//   for every cell of the w x h rectangle:
//       Level differs -> false
//       PavedRoads or PavedRoadEnds tile -> false unless allowRoads
//       otherwise: placeholder / MiscPaveTile / PaveTile must hold, else false
//   true
//
// Used by sub_58F2C0 for the 6x6 / 7x6 patches at both ends of a corridor, so
// both end pieces sit on one height.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::TileRectLevelClear(int x, int y, int w, int h,
                                            bool allowRoads)
{
    if (workCells_ == nullptr)
        return false;

    if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y))
        || !CellExists(static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y))
        || !CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y + h - 1))
        || !CellExists(static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + h - 1)))
        return false;

    const int level = CellAt(x, y)->Level;                 // 0x5a74xx
    for (int row = y; row < y + h; ++row)                  // 0x5a74xx
    {
        for (int col = x; col < x + w; ++col)
        {
            const MapCell* cell = CellAt(col, row);
            if (cell->Level != level)                      // 0x5a74xx
                return false;
            if (IsPavedRoadTile(cell) || IsPavedRoadEndTile(cell))
            {
                if (!allowRoads)
                    return false;
            }
            else if (!IsPlaceholderTile(cell)
                     && !IsMiscPaveTile(cell)
                     && !IsPaveTile(cell))
            {
                return false;
            }
        }
    }
    return true;
}

// sub_5902C0 - "the rectangle is an untouched, uniformly levelled slot"
// (0x5902c0 - 0x5904af).
//
//   level = the Level of the first cell (x, y)
//   same corner gate on {x, y, w, h}
//   for every cell of the (w + 1) x (h + 1) rectangle:   <- one cell wider/taller
//       OverlayTypeIndex != -1 or Level != level or      than the corner gate
//       (not a placeholder and not water family)      -> false
//   true
//
// Note the 1-cell slack: the vanilla's scan runs col = x .. x + w and
// row = y .. y + h (0x590425 / 0x59042f compare against x + w + 1 and
// y + h + 1), while the corner gate only covers x + w - 1 / y + h - 1.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::OverlayRectClear(int x, int y, int w, int h)
{
    if (workCells_ == nullptr)
        return false;

    const int level = CellAt(x, y)->Level;                 // 0x5902f5
    if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y))
        || !CellExists(static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y))
        || !CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y + h - 1))
        || !CellExists(static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + h - 1)))
        return false;

    for (int row = y; row < y + h + 1; ++row)              // 0x59041f
    {
        for (int col = x; col < x + w + 1; ++col)          // 0x590429
        {
            const MapCell* cell = CellAt(col, row);        // 0x59044c
            if (cell->OverlayTypeIndex != -1
                || cell->Level != level
                || (!IsPlaceholderTile(cell)               // sub_486380
                    && !IsWaterFamilyTile(cell)))          // sub_4865D0
                return false;
        }
    }
    return true;
}

// sub_5904B0 - the bridge repair hut placer (0x5904b0 - 0x5906cf).
//
//   for every cell of the (w + 1) x (h + 1) rectangle:
//       OverlayTypeIndex == -1 && placeholder tile && no object
//       -> hand the cell to CreateNeutralBridgeRepairHut and answer 1
//   false when the scan runs out
//
// The caller (sub_58F2C0) uses the answer to decide whether to search its wider
// fallback rectangle, so the scan itself is what the port must reproduce.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::PlaceBridgeRepairHut(int x, int y, int w, int h)
{
    if (workCells_ == nullptr)
        return false;

    for (int row = y; row < y + h + 1; ++row)              // 0x5904xx
    {
        for (int col = x; col < x + w + 1; ++col)
        {
            const MapCell* cell = CellAt(col, row);
            // vanilla: OverlayTypeIndex == -1 && placeholder tile && !FirstObject.
            // The port has no object chain, so "occupied" is carried by the
            // cell's AltFlags instead - CreateNeutralBridgeRepairHut sets it for
            // every hut it records, which is what keeps a second hut off the
            // same cell.
            if (cell->OverlayTypeIndex == -1 && IsPlaceholderTile(cell)
                && (cell->AltFlags & AltCellFlags_ContainsBuilding) == 0)
            {
                CreateNeutralBridgeRepairHut(
                    CellStruct{ static_cast<int16_t>(col),
                                static_cast<int16_t>(row) });    // 0x5905b2
                return true;                                     // 0x5905bb
            }
        }
    }
    return false;
}

// The bridge repair hut of sub_5904B0 (0x590548 - 0x5905c6).
//
// Vanilla resolves the owner and the type by name, then builds a live object:
//
//   owner = HouseClass::FindByCountryIndex(
//               HouseTypeClass::FindIndexOfName("Neutral"));
//   index = sub_45E7B0("CABHUT");         // BuildingTypeClass::Array name scan
//   building = new BuildingClass(0x720);
//   BuildingClass::CTOR(building, BuildingTypeClass::Array.Items[index], owner);
//   building->Unlimbo({ coords.X * 256 + 128, coords.Y * 256 + 128, 0 },
//                     DirType_North);
//
// The port takes route B (see MapStructure): no object system exists, so the
// hut is recorded in the map's building list instead. The two name lookups have
// no counterpart either - both names are fixed, and vanilla bails out when
// either fails, which is exactly the case the record cannot represent. The
// in-game coordinates Unlimbo would use are dropped: the .map [Structures]
// section stores the cell, so MapStructure keeps that.
void RandomMapGenerator::CreateNeutralBridgeRepairHut(CellStruct coords)
{
    if (!CellExists(coords.X, coords.Y))
        return;

    structures_.push_back(MapStructure(coords, "CABHUT", "Neutral House", 0 /*North*/));

    // Unlimbo's side effect that the placer of sub_5904B0 depends on: the cell
    // now reads as occupied, so no second hut is dropped on it.
    CellAt(coords.X, coords.Y)->AltFlags |= AltCellFlags_ContainsBuilding;
}

// The corridor cross-section gate of sub_58F2C0's four probe walks: the two
// cells that span the corridor - (x, y) and (x + 2, y) for a horizontal band,
// (x, y) and (x, y + 2) for a vertical one - must both lie in the usable area
// (checkLevel 1) and neither may carry a cliff-family tile (sub_4863D0). A
// failing pair stops the walk.
bool RandomMapGenerator::CorridorBandClear(int x, int y, bool horizontal)
{
    const int x2 = horizontal ? x + 2 : x;
    const int y2 = horizontal ? y : y + 2;
    const CellStruct c1{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
    const CellStruct c2{ static_cast<int16_t>(x2), static_cast<int16_t>(y2) };
    if (!IsWithinUsableArea(c1, true) || !IsWithinUsableArea(c2, true))
        return false;
    if (IsCliffFamilyTile(CellAt(c1.X, c1.Y)) || IsCliffFamilyTile(CellAt(c2.X, c2.Y)))
        return false;
    return true;
}

// sub_58F2C0 - the paved lateral-corridor builder (58F2C0.c, 0x58f2c0 -
// 0x5902bf). The water-family branch of CarveRegionRamps calls it for every
// pair (A, B) of "big" non-water regions whose Level equals both each other's
// and the water region R's, R being the region the branch is processing.
//
// Up to 200 attempts; each one:
//
//   1. pick a random work slot [RNG]: a uniform index in [0, workSide^2)
//      resampled until the slot carries R's id AND non-zero coordinates
//      (0x58f31f). The slot's packed MapCoords is the anchor (cx, cy).
//   2. grow four probe corridors out of the anchor - up (Y_1 walking down from
//      cy), down (Y walking up from cy), left (X_3 walking down from cx) and
//      right (X_2 walking up from cx). Each step stops when its 2-cell
//      cross-section leaves the usable area or hits a cliff, and each step also
//      re-tests a 3x1 (vertical corridors) / 1x3 (horizontal corridors)
//      rectangle with sub_5A7250. The vertical pair is valid (v49) when the
//      3x3 rectangles {X_1, Y_1 - 3} and {X, Y + 1} are clear and the two end
//      cells (X_1, Y_1) / (X, Y) carry exactly the marks {A->id, B->id}; the
//      horizontal pair (v50) is the mirror image with {X_3 - 3, Y_3} and
//      {X_2 + 1, Y_2} and the end cells (X_2, Y_2) / (X_3, Y_3).
//   3. when both pairs qualify the SHORTER corridor wins, the vertical one
//      losing a tie (0x58f9xx); the survivor's length must stay below
//      `tries / 25 + 8`, and its rectangle must pass sub_5902C0.
//   4. stamp the corridor: every cell takes an OverlayTypeIndex - 94 at the
//      near edge, 92 at the far edge, 74 + (x % 4) between them for the
//      horizontal corridor; 96 / 98 / 83 + (y % 4) for the vertical one - and
//      OverlayData = the distance from the near edge.
//      Then two patches are tested with sub_5A7440 (6x6 at the far end, 6x6 at
//      the near end for the horizontal corridor; 7x6 before and after for the
//      vertical one) and, when a patch passes, one 0/1 draw [RNG] picks between
//      a PavedRoads tile (pavedRoadsIndex_ + 9 / + 10 / + 12 / + 13) and its
//      PavedRoadEnds alternative (pavedRoadEndsIndex_ + 0 / + 1 / + 2 / + 3);
//      a failing patch stamps the paved-road-end tile directly. Both go through
//      sub_5A6C10 with region id and Level -1.
//      Finally two sub_5904B0 calls look for a bridge repair hut spot (narrow
//      rectangle first, wide one as the fallback).
//   5. success returns immediately; otherwise the next attempt starts.
//
// The vanilla returns the success flag; the caller discards it.
void RandomMapGenerator::LinkSameLevelRegions(MapRegion* water, MapRegion* regionA,
                                              MapRegion* regionB)
{
    if (water == nullptr || regionA == nullptr || regionB == nullptr
        || workCells_ == nullptr)
        return;

    const int idA = regionA->id;                          // 0x58f2d1 (v100)
    const int idB = regionB->id;                          // 0x58f2c9 (v69)
    const int side = size_.workSide;                      // dword_89C2DC
    const int workCount = side * side;                    // 0x58f31x (v7)
    const int waterId = water->id;                        // this[2]

    for (int tries = 0; tries < 200; ++tries)             // 0x58fa8x (v64)
    {
        // 1. the random cell of the water region (0x58f31f - 0x58f3b6)
        int idx;
        while (true)
        {
            do
            {
                idx = F2I64(                               // 0x58f347 [RNG]
                    static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                    * static_cast<double>(workCount)
                    * 2.328306437080797e-10);              // dbl_7ED898
            }
            while (idx > workCount - 1);
            if (workCells_[idx].data[14] == waterId        // 0x58f389
                && workCells_[idx].data[0] != 0)           // 0x58f3aa
                break;
        }

        const int packed = workCells_[idx].data[0];        // 0x58f3xx (v58)
        const int cx = static_cast<int16_t>(packed & 0xFFFF);
        const int cy = static_cast<int16_t>((packed >> 16) & 0xFFFF);

        int X_1 = cx - 1, Y_1 = cy;                        // the up corridor
        int X   = X_1,    Y   = Y_1;                       // the down corridor
        int X_2 = cx,     Y_2 = cy - 1;                    // the right corridor
        int X_3 = X_2,    Y_3 = Y_2;                       // the left corridor

        bool v49 = true;                                   // the vertical pair
        bool v50 = true;                                   // the horizontal pair
        bool v25 = v50;                                    // 0x58f8xx

        // 2a. the vertical pair: the up walk (0x58f450 / 0x58f4fb)
        if (!TileRectClear(X_1, Y_1, 3, 1, false, false))
        {
            while (true)
            {
                --Y_1;                                     // 0x58f476
                if (!CorridorBandClear(X_1, Y_1, true))
                    break;
                if (TileRectClear(X_1, Y_1, 3, 1, false, false))
                    break;
            }
        }

        // 2b. the down walk (0x58f552 / 0x58f5fd) and the verdict
        if (TileRectClear(X_1, Y_1 - 3, 3, 3, false, false))       // 0x58f53a
        {
            bool reached = false;
            if (TileRectClear(X, Y, 3, 1, false, false))           // 0x58f552
            {
                reached = true;
            }
            else
            {
                while (true)
                {
                    ++Y;                               // 0x58f5xx
                    if (!CorridorBandClear(X, Y, true))
                        break;
                    if (TileRectClear(X, Y, 3, 1, false, false))
                    {
                        reached = true;
                        break;
                    }
                }
            }
            if (!reached || !TileRectClear(X, Y + 1, 3, 3, false, false))  // 0x58f640
                v49 = false;
        }
        else
        {
            v49 = false;                                   // 0x58f64e
        }

        // 2c. the horizontal pair: the left walk (0x58f701)
        if (!TileRectClear(X_3, Y_3, 1, 3, false, false))
        {
            while (true)
            {
                --X_3;
                if (!CorridorBandClear(X_3, Y_3, false))
                    break;
                if (TileRectClear(X_3, Y_3, 1, 3, false, false))
                    break;
            }
        }

        // 2d. the right walk (0x58f803) and the verdict
        if (TileRectClear(X_3 - 3, Y_3, 3, 3, false, false))       // 0x58f740
        {
            bool reached = false;
            if (TileRectClear(X_2, Y_2, 1, 3, false, false))       // 0x58f803
            {
                reached = true;
            }
            else
            {
                while (true)
                {
                    ++X_2;
                    if (!CorridorBandClear(X_2, Y_2, false))
                        break;
                    if (TileRectClear(X_2, Y_2, 1, 3, false, false))
                    {
                        reached = true;
                        break;
                    }
                }
            }
            if (!reached || !TileRectClear(X_2 + 1, Y_2, 3, 3, false, false))  // 0x58f846
                v50 = false;
        }
        else
        {
            v50 = false;                                   // 0x58f8xx
        }

        // 3. the marks at the corridor ends, the lengths and the choice
        int vLen = 999;                                    // 0x58f8xx (v58)
        int n999 = 999;
        int markV20 = -1, markV21 = -1, markH67 = -1, markH22 = -1;
        if (v49)
        {
            vLen = (Y >= Y_1) ? (Y - Y_1) : (Y_1 - Y);     // abs32
            markV20 = workCells_[X_1 + side * Y_1].data[14];    // 0x58f9xx
            markV21 = workCells_[X + side * Y].data[14];
        }
        if (v50)
        {
            n999 = (X_2 >= X_3) ? (X_2 - X_3) : (X_3 - X_2);
            markH67 = workCells_[X_2 + side * Y_2].data[14];
            markH22 = workCells_[X_3 + side * Y_3].data[14];
        }
        if (v49)
        {
            // the two ends must carry A's and B's marks, either way round
            if (!((markV20 == idA && markV21 == idB)
                  || (markV20 == idB && markV21 == idA)))
                v49 = false;
        }
        if (v50)
        {
            if ((markH67 == idA && markH22 == idB)
                || (markH67 == idB && markH22 == idA))
            {
                if (v49)
                {
                    if (vLen >= n999)
                        v49 = false;                   // the horizontal one wins
                    else
                    {
                        v25 = false;                   // the vertical one wins
                        v50 = false;
                    }
                }
            }
            else
            {
                v25 = false;
                v50 = false;
            }
        }

        // 4. the corridor rectangle and the length budget (0x58f9xx)
        int X_4 = 0, Y_5 = 0, n3_8 = 0, n3_9 = 0;
        const int budget = tries / 25 + 8;                 // n999_1
        if (v25 && n999 < budget)
        {
            X_4 = X_3;                                     // 0x58f9xx
            Y_5 = Y_3;
            n3_8 = X_2 - X_3 + 1;
            n3_9 = 3;
        }
        else
        {
            if (!v49 || vLen >= budget)                    // LABEL_128
                continue;
            X_4 = X_1;
            Y_5 = Y_1;
            n3_8 = 3;
            n3_9 = Y - Y_1 + 1;
        }
        if (n3_8 <= 0 || n3_9 <= 0)                        // LABEL_128
            continue;

        if (v50)                                           // the horizontal corridor
        {
            if (!OverlayRectClear(X_4, Y_5, n3_8, n3_9))   // 0x58fa66
                continue;                                  // LABEL_128

            // the overlay strip: near edge 94, far edge 92, 74 + x % 4 between
            for (int row = Y_5; row < Y_5 + n3_9; ++row)   // 0x58fa8x - 0x58fb5x
            {
                for (int col = X_4; col < X_4 + n3_8; ++col)
                {
                    MapCell* cell = CellAt(col, row);
                    if (col == X_4)
                        cell->OverlayTypeIndex = 94;       // 0x58fb95
                    else if (col == X_4 + n3_8 - 1)
                        cell->OverlayTypeIndex = 92;       // 0x58fb99
                    else
                        cell->OverlayTypeIndex = col % 4 + 74;    // 0x58fb9d
                    cell->OverlayData = row - Y_5;
                }
            }

            // the far end patch (0x58fbd2 - 0x58fc85)
            if (TileRectLevelClear(X_4 + n3_8, Y_5 - 2, 6, 6, false))
            {
                int draw;
                do
                {
                    draw = DrawZeroOrOne();                // 0x58fbe2 [RNG] 0/1
                }
                while (draw > 1);
                if (draw != 0)
                    PlaceWaterDetailTile(pavedRoadsIndex_ + 10,     // 0x58fc85
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4 + n3_8),
                                               static_cast<int16_t>(Y_5) }),
                        -1, -1);
                else
                    PlaceWaterDetailTile(pavedRoadEndsIndex_,       // LABEL_90
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4 + n3_8),
                                               static_cast<int16_t>(Y_5) }),
                        -1, -1);
            }
            else
            {
                PlaceWaterDetailTile(pavedRoadEndsIndex_,
                    PackCoords(CellStruct{ static_cast<int16_t>(X_4 + n3_8),
                                           static_cast<int16_t>(Y_5) }),
                    -1, -1);
            }

            // the near end patch (0x58fcbd - 0x58fd57)
            if (TileRectLevelClear(X_4 - 6, Y_5 - 2, 6, 6, false))
            {
                int draw;
                do
                {
                    draw = DrawZeroOrOne();                // 0x58fccb [RNG] 0/1
                }
                while (draw > 1);
                if (draw != 0)
                    PlaceWaterDetailTile(pavedRoadsIndex_ + 9,      // 0x58fd57
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4 - 4),
                                               static_cast<int16_t>(Y_5) }),
                        -1, -1);
                else
                    PlaceWaterDetailTile(pavedRoadEndsIndex_ + 2,   // LABEL_95
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4 - 1),
                                               static_cast<int16_t>(Y_5) }),
                        -1, -1);
            }
            else
            {
                PlaceWaterDetailTile(pavedRoadEndsIndex_ + 2,
                    PackCoords(CellStruct{ static_cast<int16_t>(X_4 - 1),
                                           static_cast<int16_t>(Y_5) }),
                    -1, -1);
            }

            // the two bridge repair hut spots (0x58fd95 / 0x58fdd8 and 0x58fe14)
            if (!PlaceBridgeRepairHut(X_4, Y_5 - 1, 2, 5))
                PlaceBridgeRepairHut(X_4 - 1, Y_5 - 2, 3, 7);
            if (!PlaceBridgeRepairHut(n3_8 + X_4 - 2, Y_5 - 1, 2, 5))
                PlaceBridgeRepairHut(n3_8 + X_4 - 2, Y_5 - 2, 3, 7);

            return;                                        // LABEL_127 (v51 = 1)
        }

        if (v49)                                           // the vertical corridor
        {
            if (!OverlayRectClear(X_4, Y_5, n3_8, n3_9))   // 0x58fe84
                continue;                                  // LABEL_128

            // the overlay strip: first row 96, last row 98, 83 + y % 4 between
            for (int row = Y_5; row < Y_5 + n3_9; ++row)   // 0x58fecx - 0x5900xx
            {
                for (int col = X_4; col < X_4 + n3_8; ++col)
                {
                    MapCell* cell = CellAt(col, row);
                    if (row == Y_5)
                        cell->OverlayTypeIndex = 96;       // 0x5900xx
                    else if (row == Y_5 + n3_9 - 1)
                        cell->OverlayTypeIndex = 98;
                    else
                        cell->OverlayTypeIndex = row % 4 + 83;
                    cell->OverlayData = col - X_4;
                }
            }

            // the patch before the corridor (0x590003 - 0x5900b3)
            if (TileRectLevelClear(X_4 - 2, Y_5 - 6, 7, 6, false))
            {
                int draw;
                do
                {
                    draw = DrawZeroOrOne();                // 0x590013 [RNG] 0/1
                }
                while (draw > 1);
                if (draw != 0)
                    PlaceWaterDetailTile(pavedRoadsIndex_ + 13,     // 0x5900b3
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4),
                                               static_cast<int16_t>(Y_5 - 4) }),
                        -1, -1);
                else
                    PlaceWaterDetailTile(pavedRoadEndsIndex_ + 1,   // LABEL_117
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4),
                                               static_cast<int16_t>(Y_5 - 1) }),
                        -1, -1);
            }
            else
            {
                PlaceWaterDetailTile(pavedRoadEndsIndex_ + 1,
                    PackCoords(CellStruct{ static_cast<int16_t>(X_4),
                                           static_cast<int16_t>(Y_5 - 1) }),
                    -1, -1);
            }

            // the patch after the corridor (0x5900ee - 0x590191)
            if (TileRectLevelClear(X_4 - 2, Y_5 + n3_9, 7, 6, false))
            {
                int draw;
                do
                {
                    draw = DrawZeroOrOne();                // 0x5900fc [RNG] 0/1
                }
                while (draw > 1);
                if (draw != 0)
                    PlaceWaterDetailTile(pavedRoadsIndex_ + 12,     // 0x590191
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4),
                                               static_cast<int16_t>(n3_9 + Y_5) }),
                        -1, -1);
                else
                    PlaceWaterDetailTile(pavedRoadEndsIndex_ + 3,   // LABEL_122
                        PackCoords(CellStruct{ static_cast<int16_t>(X_4),
                                               static_cast<int16_t>(n3_9 + Y_5) }),
                        -1, -1);
            }
            else
            {
                PlaceWaterDetailTile(pavedRoadEndsIndex_ + 3,
                    PackCoords(CellStruct{ static_cast<int16_t>(X_4),
                                           static_cast<int16_t>(n3_9 + Y_5) }),
                    -1, -1);
            }

            // the two bridge repair hut spots (0x5901cf / 0x59020e and 0x59024a / 0x590290)
            if (!PlaceBridgeRepairHut(X_4 - 1, Y_5, 5, 2))
                PlaceBridgeRepairHut(X_4 - 2, Y_5 - 1, 7, 3);
            if (!PlaceBridgeRepairHut(X_4 - 1, n3_9 + Y_5 - 2, 5, 2))
                PlaceBridgeRepairHut(X_4 - 2, n3_9 + Y_5 - 2, 7, 3);

            return;                                        // LABEL_127 (v51 = 1)
        }
    }
}

// The id -> record lookup the Making-regions helpers inline: a linear scan of the
// region array comparing +8 with the id. The vanilla walks one record past the
// array when the id is absent; the port answers nullptr instead.
MapRegion* RandomMapGenerator::FindRegionById(int id) const
{
    for (size_t i = 0; i < regions_.size(); ++i)
    {
        if (regions_[i] != nullptr && regions_[i]->id == id)
            return regions_[i];
    }
    return nullptr;
}

// MapClass::IsWithinUsableArea (0x578460 - 0x57852f).
//
//   level = 0
//   if (checkLevel)
//   {
//       cell  = the cell at coords (the InvalidCell sentinel when out of range)
//       level = cell->Level
//       if (cell->SlopeIndex && coords.X + coords.Y
//                                < level + MapRect.Width + 2*VisibleRect.Y + 4)
//           ++level
//   }
//   return coords.Y + coords.X >  level + MapRect.Width + 2 * VisibleRect.Y
//       && coords.Y + coords.X <= level + MapRect.Width
//                                   + 2 * (VisibleRect.Y + VisibleRect.Height) + 2
//       && coords.X - coords.Y <  2 * (VisibleRect.X + VisibleRect.Width)
//                                   - MapRect.Width
//       && coords.Y - coords.X <  MapRect.Width - 2 * VisibleRect.X;
//
// MapRect.Width is W' (size_.mapWidth) and VisibleRect is the scenario's LocalSize
// {2, 5, W, H}, whose offsets are kVisibleOffset* below.
bool RandomMapGenerator::IsWithinUsableArea(CellStruct coords, bool checkLevel)
{
    return IsWithinUsableRect(coords, checkLevel,
                              kVisibleOffsetX, kVisibleOffsetY,
                              size_.width, size_.height);
}

// sub_578640 - the same test with the rectangle passed in (see the declaration):
// the only other caller in the generator is sub_594870, which inflates the
// rectangles by (4, 4, -8, -8).
bool RandomMapGenerator::IsWithinUsableRect(CellStruct coords, bool checkLevel,
                                            int visX, int visY,
                                            int visW, int visH)
{
    if (cellSlots_ == nullptr)
        return false;

    int level = 0;                                        // 0x578460
    if (checkLevel)
    {
        MapCell* cell = CellAt(coords.X, coords.Y);
        level = cell->Level;
        if (cell->SlopeIndex != 0                          // 0x5784a9
            && coords.Y + coords.X
                   < level + size_.mapWidth + 2 * visY + 4)
            ++level;
    }

    const int width = size_.mapWidth;                     // 0x5784e0 MapRect.Width
    return coords.Y + coords.X > level + width + 2 * visY
        && coords.Y + coords.X
               <= level + width + 2 * (visY + visH) + 2
        && coords.X - coords.Y < 2 * (visX + visW) - width
        && coords.Y - coords.X < width - 2 * visX;
}

// sub_5A19E0 - cliff edge / corner Level fix-up.
//   Walk the diamond; for every cell ask CliffMask (sub_579B70) for its cliff
//   shape mask and handle the three corner patterns it can answer:
//     131 = N|NE|E -> the EAST neighbour's Level -= 4
//      56 = S|SW|W -> the WEST neighbour's Level -= 4
//     224 = W|NW|N -> the cell ITSELF          += 4
//   The 131 / 56 branches also need the SE neighbour's mask to match and a
//   third cell to be a placeholder (sub_486380 = IsPlaceholderTile), and then
//   win a 0/1 draw that must come out 0 (one Random() draw; F2I64(Random() *
//   2^-31) is 0 or 1, so the vanilla's rejection loop never repeats). The 224
//   branch has NO draw.
//   The adjusted cell then receives a work[+56] region mark: 131 / 56 copy the
//   walk cell's own mark, 224 copies the WEST neighbour's.
// Decompile: 5A19E0.c (579B70.c, 486380.c).
void RandomMapGenerator::FixCliffLevels()
{
    if (workCells_ == nullptr || cellSlots_ == nullptr)
        return;

    // work[+56] of a cell - the 20 * X + 20 * W' * Y + 14 slot of the vanilla's
    // work array (dword_ABED10), i.e. our WorkAt(x, y).data[14].
    auto mark = [this](const MapCell* c) -> int&
    {
        const int x = static_cast<int16_t>(c->MapCoords & 0xFFFF);
        const int y = static_cast<int16_t>((uint32_t)c->MapCoords >> 16);
        return WorkAt(x, y).data[14];
    };

    CellIterator it;                                      // 0x5a19e9 sub_578350
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())                     // 0x5a19f3
    {
        MapCell* southWest = GetNeighbourCell(cell, 5);   // 0x5a1a12
        MapCell* northEast = GetNeighbourCell(cell, 1);   // 0x5a1a1f
        MapCell* southEast = GetNeighbourCell(cell, 3);   // 0x5a1a2c
        MapCell* east      = GetNeighbourCell(cell, 2);   // 0x5a1a37
        MapCell* west      = GetNeighbourCell(cell, 6);   // 0x5a1a44

        const int mask = CliffMask(cell);                 // 0x5a1a46 sub_579B70

        if (mask == 131)
        {
            if (CliffMask(southEast) != 131 || !IsPlaceholderTile(east))
                continue;                                 // 0x5a1a6e
            if (DrawZeroOrOne())                          // 0x5a1a85 - 0x5a1aa7
                continue;

            east->Level -= 4;                             // 0x5a1ab6
            mark(east) = mark(cell);                      // 0x5a1c45
        }
        else if (mask == 56)
        {
            if (CliffMask(southEast) != 56 || !IsPlaceholderTile(west))
                continue;                                 // 0x5a1b1f
            if (DrawZeroOrOne())                          // 0x5a1b36 - 0x5a1b58
                continue;

            west->Level -= 4;                             // 0x5a1b67
            mark(west) = mark(cell);                      // 0x5a1c45
        }
        else if (mask == 224)
        {
            if (CliffMask(southWest) != 224 || CliffMask(northEast) != 224
                || !IsPlaceholderTile(cell))
                continue;                                 // 0x5a1bea

            cell->Level += 4;                             // 0x5a1bfc
            mark(cell) = mark(west);                      // 0x5a1c45
        }
    }
}

// sub_578E60 - cliff placement driver. The vanilla call is sub_578E60(0, -1)
// (__stdcall, so the `mov ecx, MouseClass::Instance` right before it is dead):
// a1 = 0 is unused by the body and a2 = -1 is the mode the main flow passes, so
// both subordinate calls get mode = -1.
//   1. If the work array (dword_ABED10 = our workCells_) is missing, allocate
//      workSide^2 80-byte cells and construct each one (sub_58BDC0 = the
//      WorkCell constructor); remember that this call owns the array so it is
//      freed again on the way out.
//   2. Set the enable byte +74 of every work cell (sub_58C2C0 = WorkAtLinear).
//   3. Reset the foundation preview (sub_4A8BF0 = ResetPreviewState).
//   4. Pass 1 - walk the diamond and run sub_579010 (our CliffPass) per cell;
//      a 0 answer stops the walk.
//   5. Pass 2 - walk it again and run sub_579620 (our PlaceCliffPiece) on every
//      cell with SlopeIndex == 0 that sub_486380 (IsPlaceholderTile) accepts;
//      again a 0 answer stops the walk.
//   6. Clear the isotile selection and tear MouseClass::CurrentBuilding down.
//   7. Free the work array when step 1 allocated it.
// Decompile: 578E60.c (58C2C0.c, 579620.c).
void RandomMapGenerator::PlaceCliffs()
{
    if (cellSlots_ == nullptr)
        return;

    // Port extension: fresh per-map footprint outcomes for RepairCliffPieces.
    cliffStamps_.clear();

    const int workCount = size_.workSide * size_.workSide;

    // ---- 1. Work array, on demand (0x578e6x) ------------------------------
    bool allocatedHere = false;                           // vanilla v12
    if (workCells_ == nullptr)
    {
        workCells_ = new WorkCell[workCount];             // sub_58BDC0 per cell
        allocatedHere = true;
    }

    // ---- 2. Enable every work cell (0x578exx) -----------------------------
    for (int i = 0; i < workCount; ++i)
        WorkAtLinear(i).Byte(74) = 1;                     // sub_58C2C0

    // ---- 3. Preview reset (sub_4A8BF0) ------------------------------------
    ResetPreviewState();

    bool pass = true;                                     // vanilla v2

    // ---- 4. Pass 1: fill the depressions (sub_579010) ---------------------
    {
        CellIterator it;                                  // sub_578350
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            if (!pass)
                break;
            pass = CliffPass(cell, -1);                   // sub_579010
        }
    }

    // ---- 5. Pass 2: one cliff piece per placeholder cell (sub_579620) -----
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            if (!pass)
                break;
            if (cell->SlopeIndex == 0 && IsPlaceholderTile(cell))
                pass = PlaceCliffPiece(cell, -1);         // sub_579620
        }
    }

    // ---- 6. Teardown (0x578f3x) -------------------------------------------
    // The vanilla also destroys MouseClass::CurrentBuilding here. The port has
    // no live object system (same note as SmoothWaterBody's), so only the
    // isotile selection cursor is cleared.
    currentBuildingType_ = 0;

    // ---- 7. Release the array if step 1 allocated it (0x578f4x) -----------
    if (allocatedHere)
    {
        delete[] workCells_;
        workCells_ = nullptr;
    }
}

// sub_579620 - single cliff-piece placement (579620.c, 0x579620 - 0x579b68).
//
// Called by the driver above for one placeholder cell with SlopeIndex == 0.
// Resolves which cliff tile the cell's neighbourhood calls for - the slot
// `slot`, 1..40, inside the CliffSet family - and stamps it through sub_57B440
// (our PlaceIsoTile).
//
//   roll = sub_598030(0, 5)      one RNG draw, our rng_.RandomFloatRange(0, 5)
//   mask = sub_579B70(cell)      our CliffMask
//
// The mask has one bit per neighbour that sits exactly one Level higher
// (N 0x80, NE 0x01, E 0x02, SE 0x04, S 0x08, SW 0x10, W 0x20, NW 0x40). The
// branches test it against the 2- and 3-direction patterns a cliff tile can
// face and pick the slot; some paths also consult the S / E / SE neighbours'
// own masks. Slots 1, 2, 8, 18, 21, 22, 26, 33, 34, 38, 39 and 40 are fixed;
// the rest are `roll % 3 + <base>` variants, and two paths pick between two
// slots with a `roll & 1` coin.
//
// Placement tail (0x579a0x - 0x579b68):
//   - the isotile is CliffSet + slot - 1. The vanilla reads it as
//     &IsometricTileTypeClass::Array.Items[IsoTileTypeIndex_0 - 1 + slot],
//     IsoTileTypeIndex_0 being the CliffSet global @0xAA1020 - our
//     shoreTileIndex_ - so the selection is shoreTileIndex_ + slot - 1, the
//     same shape SelectShoreTile uses with shorePieces_.
//   - the foundation anchor is the cell plus word_ABDDA4[slot] (an int16 X/Y
//     pair table). That table has NO writer anywhere in gamemd.exe - its only
//     xref is the read right here - so every entry is 0 and the anchor is the
//     cell itself, exactly as shoreAnchor_ is treated in SelectShoreTile.
//   - the tile is stamped with PlaceIsoTile(0, 0, cell->Level, genCode,
//     &placed, 0) with `placed` pre-set to true, and the answer is
//     `ok || placed` (vanilla: `sub_57B440(...) || LOBYTE(flag)`).
//
// The neighbour lookups go through CellClass::GetNeighbourCell (0x481810 =
// our GetNeighbourCell); an out-of-diamond cell becomes MapClass::InvalidCell,
// whose CliffMask then answers 0 through its own diamond gate - which is what
// the vanilla's InvalidCell fallback produces here too.
//
// Returns false to stop the driver's walk.
bool RandomMapGenerator::PlaceCliffPiece(MapCell* cell, int genCode)
{
    const int roll = rng_.RandomFloatRange(0, 5);         // 0x579630 sub_598030
    const int mask = CliffMask(cell);                     // 0x579645 sub_579B70

    int slot = 0;                                         // vanilla n34

    if ((mask & 0xA0) == 0xA0)                            // 0x579652
    {
        const int southMask = CliffMask(GetNeighbourCell(cell, 4));   // (x, y+1)
        const int eastMask  = CliffMask(GetNeighbourCell(cell, 2));   // (x+1, y)
        // 0x5796f9 test bl,20h ; 0x579702 test al,80h - the second probe is
        // the east neighbour's N bit, NOT a signed-byte test. CliffMask only
        // ever returns 0..255, so an "eastMask < 0" port was dead code and
        // sent these inner-corner cells to the random 9/10/11 pieces instead
        // of the fixed corner piece 34, leaving a one-cell wall gap between
        // the neighbouring cliff pieces.
        if ((southMask & 0x20) != 0 || (eastMask & 0x80) != 0)
            slot = 34;
        else
            slot = roll % 3 + 9;
        if (slot <= 0)                                    // LABEL_81
            return true;
        goto place;
    }

    if ((mask & 0x82) == 0x82) { slot = 39; goto place; } // N | E
    if ((mask & 0x0A) == 0x0A) { slot = 33; goto place; } // SE | S
    if ((mask & 0x28) == 0x28) { slot = 40; goto place; } // S | W

    if ((mask & 2) == 0)                                  // E bit clear
    {
        if ((mask & 0x20) != 0)                           // W set
        {
            const int southMask = CliffMask(GetNeighbourCell(cell, 4));
            if ((mask & 0x10) == 0 || (mask & 0xC) != 0
                || (southMask & 0x28) == 0x28)
            {
                slot = 18;
                goto place;
            }
            slot = roll % 3 + 15;
        }
        else if ((mask & 8) != 0)                         // S set
        {
            const int eastMask = CliffMask(GetNeighbourCell(cell, 2));
            const int seMask   = CliffMask(GetNeighbourCell(cell, 3));
            if ((mask & 4) == 0 || (mask & 3) != 0 || (eastMask & 0xA) == 0xA)
            {
                if ((mask & 6) != 0 || (seMask & 8) != 0 || (roll & 1) == 0)
                    slot = 26;
                else
                    slot = 21;
                goto place;
            }
            slot = roll % 3 + 23;
        }
        else if ((mask & 0x80) != 0)                      // N set
        {
            const int eastMask = CliffMask(GetNeighbourCell(cell, 2));
            if ((mask & 1) == 0 || (mask & 6) != 0 || (eastMask & 0x82) == 0x82)
            {
                slot = 8;
                goto place;
            }
            slot = roll % 3 + 5;
        }
        else
        {
            goto tail;                                    // LABEL_73
        }

        if (slot <= 0)                                    // LABEL_44 -> LABEL_81
            return true;
        goto place;
    }

    // ---- E bit set --------------------------------------------------------
    {
        const int southMask = CliffMask(GetNeighbourCell(cell, 4));
        const int seMask    = CliffMask(GetNeighbourCell(cell, 3));
        if ((mask & 4) != 0 && (mask & 0x18) == 0 && (southMask & 0xA) != 0xA)
        {
            slot = roll % 3 + 35;
            if (slot <= 0)                                // LABEL_44 -> LABEL_81
                return true;
            goto place;
        }
        if ((mask & 0xC) != 0 || (seMask & 2) != 0 || (roll & 1) == 0)
            slot = 38;
        else
            slot = 1;
        goto place;
    }

tail:
    // LABEL_73 - the shared tail for a cell with neither E, W, S nor N set.
    if ((mask & 1) != 0) { slot = 2; goto place; }        // NE
    if ((mask & 4) != 0)
        slot = (roll & 1) + 29;                           // SE
    else
    {
        if ((mask & 0x10) != 0) { slot = 22; goto place; }  // SW
        if ((mask & 0x40) == 0)                           // no NW either
            return true;
        slot = roll % 3 + 12;
    }
    if (slot <= 0)                                        // LABEL_81
        return true;

place:
    // 多格"纯墙条"只剩一格能落时，撞坡修剪会把幸存格换成内角片 C34，朝向
    // 不对、和两侧崖线接不上。这里在落片前直接改选与墙面同向的单格墙头片：
    //   - 竖墙 35/36/37（1×2，往南再落一格）：南格被坡道等非占位艺术占住 → C38
    //   - 横墙 23/24/25（2×1，往东再落一格）：东格被占住 → C26（朝南单格墙）
    // 竖墙案例：(75,82) 北接 C37、南让坡道 t391，本该 C38 被剪成了 C34；
    // 横墙案例：(86,63) 东邻 (87,63) 被坡道占住，本该 C26 被剪成了 C34b。
    {
        const int16_t sx = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t sy = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);
        if (slot >= 35 && slot <= 37)
        {
            const int16_t southY = static_cast<int16_t>(sy + 1);
            if (CellExists(sx, southY) && !IsPlaceholderTile(CellAt(sx, southY)))
                slot = 38;
        }
        else if (slot >= 23 && slot <= 25)
        {
            const int16_t eastX = static_cast<int16_t>(sx + 1);
            if (CellExists(eastX, sy) && !IsPlaceholderTile(CellAt(eastX, sy)))
                slot = 26;
        }
    }
    // Port guard: refuse a piece whose wall would ride down a ramp body. The
    // surviving footprint cells are still stamped as smaller CliffSet pieces
    // (C8 / C34 / C12-14), so a ramp-top seam keeps its facade instead of
    // being flattened to t0.
    if (CliffPieceHitsRamp(cell, slot))
    {
        const int trimmed = TrimCliffPieceForRamp(cell, slot);
        if (trimmed > 0)
            return true;
        // Every wall cell of this piece rides a ramp or is already taken by
        // ramp art. The ramp itself provides the elevation transition, so a
        // CliffSet wall here would only overlap the ramp and create visual
        // garbage. Place nothing.
        const int cx = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int cy = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);
        DiagLog("CLIFF-TRIM-SKIP slot=%d anchor=(%d,%d) L%d",
                slot, cx, cy, cell->Level);
        return true;
    }
    currentBuildingType_ = shoreTileIndex_ + slot - 1;    // CliffSet + slot - 1
    SetFoundationCenter(CellStruct{                       // anchor table is all 0,
        static_cast<int16_t>(cell->MapCoords & 0xFFFF),   // so the anchor is the
        static_cast<int16_t>((uint32_t)cell->MapCoords >> 16) });  // cell itself
    {
        bool placed = true;                               // LOBYTE(flag) = 1
        return PlaceIsoTile(0, 0, cell->Level, genCode, &placed, 0) || placed;
    }
}

// sub_5A1350 - the corrected CliffSet tile for one cliff tile (5A1350.c,
// 0x5a1350 - 0x5a1775). Called by the correction pass below.
//
// The argument is a tile index inside the CliffSet family and the switch is
// keyed on the family slot (tile - shoreTileIndex_, vanilla IsoTileTypeIndex_0).
// Slots 4-6, 8-10, 11-13, 14-16 and 22-24 are five 3-slot clusters: each
// answers one of the OTHER two members of its own cluster by a roll, so the
// pass swaps a cliff piece for its rotated / flipped mate. Slots 28 and 29
// simply swap with each other, 34-36 map inside 34-36 through sub_598030, and
// every other slot answers itself (no change).
//
// RNG: exactly one draw on slots 4-6, 8-10, 11-13, 14-16, 22-24 and 34-36,
// none on 28, 29 or the default branch. The `while (x > 1 / > 2)` the vanilla
// wraps around each draw can never repeat - the scale keeps the quotient in
// {0, 1}, or in {1, 2} where 1.0 is added before the truncation.
int RandomMapGenerator::ResolveCliffVariant(int tile)
{
    const int slot = tile - shoreTileIndex_;              // vanilla IsoTileTypeIndex_0

    // F2I64(Random() * 2^-31 + 1.0) - the {1, 2} draw the slots that add 1.0
    // use (vanilla v20/v23/... are truncated only after the +1.0).
    auto drawOneOrTwo = [this]() -> int
    {
        return F2I64(static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                     * 4.656612874161595e-10 + 1.0);
    };

    switch (slot)
    {
    case 4:  return shoreTileIndex_ + drawOneOrTwo() + 4;        // +5 / +6
    case 5:  return shoreTileIndex_ + 2 * DrawZeroOrOne() + 4;   // +4 / +6
    case 6:  return shoreTileIndex_ + DrawZeroOrOne() + 4;       // +4 / +5
    case 8:  return shoreTileIndex_ + drawOneOrTwo() + 8;        // +9 / +10
    case 9:  return shoreTileIndex_ + 2 * DrawZeroOrOne() + 8;   // +8 / +10
    case 10: return shoreTileIndex_ + DrawZeroOrOne() + 8;       // +8 / +9
    case 11: return shoreTileIndex_ + drawOneOrTwo() + 11;       // +12 / +13
    case 12: return shoreTileIndex_ + 2 * DrawZeroOrOne() + 11;  // +11 / +13
    case 13: return shoreTileIndex_ + DrawZeroOrOne() + 11;      // +11 / +12
    case 14: return shoreTileIndex_ + drawOneOrTwo() + 14;       // +15 / +16
    case 15: return shoreTileIndex_ + 2 * DrawZeroOrOne() + 14;  // +14 / +16
    case 16: return shoreTileIndex_ + DrawZeroOrOne() + 14;      // +14 / +15
    case 22: return shoreTileIndex_ + drawOneOrTwo() + 22;       // +23 / +24
    case 23: return shoreTileIndex_ + 2 * DrawZeroOrOne() + 22;  // +22 / +24
    case 24: return shoreTileIndex_ + DrawZeroOrOne() + 22;      // +22 / +23
    case 28: return shoreTileIndex_ + 29;                        // plain swap
    case 29: return shoreTileIndex_ + 28;                        // plain swap
    case 34: return shoreTileIndex_ + rng_.RandomFloatRange(1, 2) + 34;      // +35 / +36
    case 35: return shoreTileIndex_ + 2 * rng_.RandomFloatRange(0, 1) + 34;  // +34 / +36
    case 36: return shoreTileIndex_ + rng_.RandomFloatRange(0, 1) + 34;      // +34 / +35
    default: return tile;                                        // unchanged
    }
}

// sub_5A17F0 - cliff correction pass (5A17F0.c, 0x5a17f0 - 0x5a19d3).
//
// Walks the diamond. For every cell whose tile is a CliffSet one (in
// [shoreTileIndex_, +40)) it fetches the S (GetNeighbourCell 4) and E
// (GetNeighbourCell 2) neighbours and, whenever a neighbour carries the SAME
// tile while sitting LOWER (cell->Height > neighbour->Height), asks sub_5A1350
// (our ResolveCliffVariant) which tile the pair really calls for. When the
// answer differs from the current tile, the replacement is stamped through
// sub_5A6C10 (our PlaceWaterDetailTile) anchored at the neighbour's own base
// cell:
//   CellsInX = the family's column count, Array.Items[tile]->CellsInX (our
//              CliffCellsInX: kCliffFootprints[slot].w). Height is encoded as
//              col + row * CellsInX, so inverting it and subtracting from the
//              neighbour's coords yields the tile's top-left cell.
//   PlaceWaterDetailTile(newTile, packed(baseCell), work(baseCell).data[14], -1)
// The work generation mark comes from the work array (dword_ABED10 = our
// workCells_); the vanilla answers -1 when that array is missing, which the
// null check below mirrors.
//
// The S and E neighbours are corrected independently and in that order, so one
// cell can be corrected twice in a single visit.
// Decompile: 5A17F0.c (5A1350.c).
void RandomMapGenerator::CorrectCliffTiles()
{
    if (cellSlots_ == nullptr)
        return;

    // One replacement: `neighbour` carries the same cliff tile but sits lower.
    auto stamp = [this](const MapCell* trigger, int dir,
                        const MapCell* neighbour, int tile)
    {
        const int newTile = ResolveCliffVariant(tile);    // sub_5A1350
        if (newTile == tile)
            return;

        const int cellsInX = CliffCellsInX(tile);         // Array.Items[tile]->CellsInX
        const int baseX = static_cast<int16_t>(neighbour->MapCoords & 0xFFFF);
        const int baseY = static_cast<int16_t>((uint32_t)neighbour->MapCoords >> 16);
        const int cellX = baseX - neighbour->Height % cellsInX;
        const int cellY = baseY - neighbour->Height / cellsInX;

        const int packed = (cellX & 0xFFFF) | (cellY << 16);
        int mark = -1;                                    // vanilla: dword_ABED10 ? ... : -1
        if (workCells_ != nullptr)
        {
            // The vanilla indexes the work array raw here. Every stamped cliff
            // cell's Height is its offset inside the tile, so baseCell lands on
            // that tile's anchor cell and the index stays inside the array - but
            // a malformed Height would produce a negative row, so the range is
            // checked rather than trusted.
            const int index = cellX + size_.workSide * cellY;
            if (index >= 0 && index < size_.workSide * size_.workSide)
                mark = workCells_[index].data[14];
        }

        PlaceWaterDetailTile(newTile, packed, mark, -1);  // sub_5A6C10
    };

    CellIterator it;                                      // sub_578350
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())
    {
        const int tile = cell->IsoTileTypeIndex;
        if (tile < shoreTileIndex_ || tile >= shoreTileIndex_ + 40)
            continue;                                     // not a CliffSet tile

        const MapCell* south = GetNeighbourCell(cell, 4); // (x, y+1)
        const MapCell* east  = GetNeighbourCell(cell, 2); // (x+1, y)

        if (tile == south->IsoTileTypeIndex && cell->Height > south->Height)
            stamp(cell, 4, south, tile);
        if (tile == east->IsoTileTypeIndex && cell->Height > east->Height)
            stamp(cell, 2, east, tile);
    }
}

// sub_59B740 - fill the leftover placeholders around green ground (59B740.c,
// 0x59b740 - 0x59b938). The Making-regions tail: MakeRegions calls it for every
// land type (0x598e1a), after the five cliff steps.
//
//   1. Walk the diamond. For every green-ground cell (sub_4867B0 =
//      IsGreenGroundTile) append each of its four orthogonal neighbours
//      (facing 0, 2, 4, 6) to a list when that neighbour is still a placeholder
//      (sub_486380 = IsPlaceholderTile). The vanilla collects into a
//      DynamicVectorClass<CellClass*> built by sub_5AD820 (growth 10000), and
//      because there is no de-duplication a placeholder reachable from several
//      green cells lands in the list more than once.
//   2. target = count / 3, capped at 1000.
//   3. Repeat `target` times:
//        - pick a random list index: one RNG draw, rejection while > count - 1
//          (the scale keeps the quotient inside [0, count], so the reject is a
//          2^-32 corner and effectively never happens);
//        - take that entry and remove it ORDER-PRESERVINGLY - the vanilla
//          shifts the following entries down one slot, it does not swap the
//          last entry into the hole;
//        - stamp the picked cell with the GreenTile base (IsoTileTypeIndex_1
//          @0xAA0E18 = our greenTileIndex_);
//        - append that cell's own placeholder orthogonal neighbours, so the
//          fill spreads outwards.
//
// RNG: exactly one draw per filled cell. Step 1 consumes none.
// Decompile: 59B740.c.
void RandomMapGenerator::FillGreenPlaceholders()
{
    if (cellSlots_ == nullptr)
        return;

    // The vanilla's DynamicVectorClass<CellClass*> (sub_5AD820); a std::vector
    // reproduces its grow-on-demand behaviour without the 10000-step growth.
    std::vector<MapCell*> pending;

    // ---- 1. Seed the list from the green-ground cells ---------------------
    {
        CellIterator it;                                  // 0x59b7xx sub_578350
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            if (!IsGreenGroundTile(cell))                 // sub_4867B0
                continue;
            for (int facing = 0; facing < 8; facing += 2) // N, E, S, W
            {
                MapCell* nb = GetNeighbourCell(cell, facing);
                if (IsPlaceholderTile(nb))                // sub_486380
                    pending.push_back(nb);
            }
        }
    }

    // ---- 2. How many to fill (0x59b8xx) -----------------------------------
    int target = static_cast<int>(pending.size()) / 3;    // 0x59b8xx
    if (target > 1000)
        target = 1000;                                    // 0x59b8xx

    // ---- 3. Fill, spreading outwards (0x59b8xx) ---------------------------
    for (int n = 0; n < target; ++n)
    {
        const int count = static_cast<int>(pending.size());

        int index;
        do
        {
            index = F2I64(static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                          * count * kUnitScale);          // 0x59b8xx
        }
        while (index > count - 1);

        MapCell* cell = pending[static_cast<size_t>(index)];
        pending.erase(pending.begin() + index);           // order-preserving

        cell->IsoTileTypeIndex = greenTileIndex_;         // IsoTileTypeIndex_1 @0xAA0E18

        for (int facing = 0; facing < 8; facing += 2)
        {
            MapCell* nb = GetNeighbourCell(cell, facing);
            if (IsPlaceholderTile(nb))
                pending.push_back(nb);
        }
    }
}
