// ============================================================================
// MapGenTiberium.cpp - the "RMG: Creating tiberium" stage
// (sub_598960 @ 0x598EE5 - 0x598FB7), its body sub_5A23A0 and the field grower
// sub_5A28C0.
//
// Stage slice:
//
//   0x598EE5  push "RMG: Creating tiberium\n", nullsub_1    debug
//   0x598EF2  ecx = this
//   0x598EF4  sub_5A23A0(this)                  // place the ore / gem fields
//   0x598EF9  if (psub_48D1D0) psub_48D1D0()    // UI callback, not ported
//   0x598F04  -- region-system teardown (the stage tail) --
//             for (every work cell) { data[14] = -1; data[15] = -1; }
//             for (every region, reverse order) ReleaseRegionObject(r, true)
//   0x598FBD  dword_ABED14 = 0                  // region-id counter
//
// Full annotated walkthrough: 598EE5_CreatingTiberium.c, 5A23A0.c and 5A28C0.c
// in the decompile folder.
//
// ----------------------------------------------------------------------------
// sub_5A23A0 - per-region ore / gem field placement
// ----------------------------------------------------------------------------
//   Ore or gem (0x5a23b4):
//     landType 0          -> gems = (this[16] == 3)
//     landType 1 / 3 / 4  -> gems = (this[16] != 3)
//     landType 2          -> gems = 1
//     other               -> gems = 0
//
//   this[16] is the RMG dialog's ore-amount combo (combo 0x408), so it maps
//   straight onto MapGenConfig::oreDensity. The related values, both verified in
//   the exe:
//     this[21] = 20 * this[16]                       (0x59730b, sub_597260)
//     this[175] = [General] RMGMinimumTiberium       (0x5982bb, ctor 2500)
//     this[176] = [General] RMGMaximumTiberium       (0x5982d5, ctor 5500)
//   and the per-starting-point cell budget is
//     base = F2I64(this[21] * 0.01 * (this[176] - this[175]) + this[175])
//     n    = F2I64(base * max(startingPoints, 0.5))
//
//   `offset` (vanilla v45) walks the waypoint list: it advances only for the
//   regions that actually grow a field.
//
// ----------------------------------------------------------------------------
// sub_5A28C0 - one field
// ----------------------------------------------------------------------------
//   sub_5A28C0(seed, target, mark, variant) grows a best-first flood over
//   placeholder cells until `target` cells carry ore:
//     pool   = 10 * target 8-byte nodes (coords + float priority)
//     heap   = min-heap over pool indices, capacity 10 * target (sub_5AD870's
//              shape: 1-based, sift-up inline, sift-down on pop)
//     at most 10 seed rounds; a round re-seeds when the heap runs dry
//     priority = |cell - anchor| + Random() * 5 * kUnitScale
//     work byte +0x3C (data[15]) is stamped with `mark` for every covered cell
//
//   `variant` picks the overlay family - true -> 27 (the gem floor), false ->
//   102 (the ore floor), each 12 variants - AND gates the ore decoration tree.
//
//   NOTE the quirk that the two call sites give the 4th argument DIFFERENT
//   meanings (verified in the disassembly at 0x5a262d and 0x5a2827):
//     pass 1 (pick-list cells)  passes `c == chosen`
//     pass 2 (starting points)  passes the gem flag
//   `chosen` is only computed when gems is set, so pass 1 passes 0 for every
//   cell on an ore map. This is vanilla behaviour, reproduced as-is.
//
//   Route B: the ore decoration tree is an engine object vanilla creates with
//   a sub_71DD80 name lookup plus the sub_71BB90 constructor; the port records
//   a MapTerrainObject instead (the .map [Terrain] source).
// ============================================================================

#include "pch.h"
#include "MapGen.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace
{
    // One 8-byte pool node: the packed cell coords and the float priority.
    struct TiberiumNode
    {
        CellStruct coords;
        float      priority;
    };

    const int16_t kDirX[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
    const int16_t kDirY[8] = { -1, -1,  0,  1,  1,  1,  0, -1 };
}

// ---------------------------------------------------------------------------
// sub_5A23A0 (0x5A23A0 - 0x5A289D)
// ---------------------------------------------------------------------------
void RandomMapGenerator::AddTiberium()
{
    if (workCells_ == nullptr)
        return;

    const int landType = static_cast<int>(config_.landType);   // this[15]
    const int control = config_.oreDensity;                    // this[16]

    // 0x5a23b4 - 0x5a23e9
    bool gems;
    switch (landType)
    {
    case 0: gems = (control == 3); break;
    case 1: case 3: case 4: gems = (control != 3); break;
    case 2: gems = true; break;
    default: gems = false; break;
    }

    // 0x5a254c - 0x5a259e
    const int this21 = 20 * control;                           // 0x59730b
    const int base = F2I64(static_cast<double>(this21) * 0.01
                           * static_cast<double>(settings_.MaxTiberium
                                                 - settings_.MinTiberium)
                           + static_cast<double>(settings_.MinTiberium));

    const int side = size_.workSide;
    int offset = 0;                                            // v45

    for (size_t i = 0; i < regions_.size(); ++i)
    {
        MapRegion* region = regions_[i];
        if (region == nullptr || region->startPointPicks.empty())   // 0x5a240f
            continue;

        int chosen = -1;                                       // v50

        if (gems)
        {
            // Anchor (0x5a242e - 0x5a24cc). startingPoints > 0: the average of
            // the region's waypoints. Otherwise a random cell of the region's
            // own cell list - a path that cannot produce a field (n becomes 0).
            CellStruct anchor{ 0, 0 };
            if (region->startingPoints <= 0)
            {
                const int cnt = static_cast<int>(region->cells.size());
                if (cnt <= 0)
                    continue;                                  // port guard
                int idx;
                do
                {
                    idx = F2I64(static_cast<double>(rng_.Next())
                                * static_cast<double>(cnt) * kUnitScale);
                }
                while (idx > cnt - 1);
                anchor = region->cells[static_cast<size_t>(idx)];
            }
            else
            {
                long sx = 0;
                long sy = 0;
                for (int w = 0; w < region->startingPoints; ++w)
                {
                    const CellStruct wc =
                        waypoints_[static_cast<size_t>(offset + w)];
                    sx += wc.X;
                    sy += wc.Y;
                }
                anchor.X = static_cast<int16_t>(sx / region->startingPoints);
                anchor.Y = static_cast<int16_t>(sy / region->startingPoints);
            }

            // Nearest pick-list entry to the anchor (0x5a24cf - 0x5a2546).
            int best = 500000;
            for (size_t c = 0; c < region->startPointPicks.size(); ++c)
            {
                const CellStruct p = region->startPointPicks[c];
                const double dx = static_cast<double>(p.X - anchor.X);
                const double dy = static_cast<double>(p.Y - anchor.Y);
                const double d = std::sqrt(dx * dx + dy * dy);
                if (d < best)
                {
                    best = F2I64(d);
                    chosen = static_cast<int>(c);
                }
            }
        }

        // Amount (0x5a2575 - 0x5a259e)
        const double sp = static_cast<double>(region->startingPoints);
        const int n = F2I64(static_cast<double>(base) * ((sp < 0.5) ? 0.5 : sp));
        const int count = static_cast<int>(region->startPointPicks.size());
        if (count == 0 || n == 0)                              // 0x5a25b4
            continue;

        // Pass 1 (0x5a25bd - 0x5a2638): one field per pick-list cell.
        const int per = n / count;
        for (int c = 0; c < count; ++c)
        {
            double jitter;
            do
            {
                jitter = rng_.Gaussian() * 50.0;
            }
            while (jitter < -100.0 || jitter > 100.0);          // 0x5a25e8/0x5a25f9

            const int amount = F2I64(static_cast<double>(per) + jitter);
            if (amount >= 0)                                   // 0x5a260a
                GrowTiberiumField(region->startPointPicks[static_cast<size_t>(c)],
                                  amount, offset + c + 1, c == chosen);
        }

        // Pass 2 (0x5a2643 - 0x5a2846): one field per starting point, its size
        // scaled by how far that waypoint sits from the field cluster.
        std::vector<double> avg;
        avg.reserve(static_cast<size_t>(region->startingPoints));
        for (int w = 0; w < region->startingPoints; ++w)
        {
            const CellStruct wc = waypoints_[static_cast<size_t>(offset + w)];
            double sum = 0.0;
            for (int c = 0; c < count; ++c)
            {
                const CellStruct p =
                    region->startPointPicks[static_cast<size_t>(c)];
                const double dx = static_cast<double>(p.X - wc.X);
                const double dy = static_cast<double>(p.Y - wc.Y);
                sum += std::sqrt(dx * dx + dy * dy);
            }
            avg.push_back(sum / static_cast<double>(count));    // 0x5a2719
        }

        double minAvg = 9999999.0;                             // 0x5a2788
        for (size_t k = 0; k < avg.size(); ++k)
        {
            if (avg[k] < minAvg)
                minAvg = avg[k];
        }

        // 0x5a27c6 - 0x5a27da
        const bool variant = (control == 3 && (landType == 1 || landType == 3
                                               || landType == 4));

        for (int w = 0; w < region->startingPoints; ++w)
        {
            const int amount =
                F2I64((avg[static_cast<size_t>(w)] - minAvg) * 15.0) + 500;
            GrowTiberiumField(waypoints_[static_cast<size_t>(offset + w)],
                              amount, offset + w + 1, variant);  // 0x5a2816
        }

        offset += region->startingPoints;                      // 0x5a2846
    }

    // [port-only] 群岛：矿柱改由 PlaceArchipelagoOrePillars 统一按"每点相同"放置，
    // GrowTiberiumField 已不再往 terrainObjects_ 里塞 TIBTRE。
    if (landType == static_cast<int>(RmLandType::Archipelago))
        PlaceArchipelagoOrePillars();

    // ---- the stage tail (0x598f04 - 0x598fbd) -----------------------------
    // Clear the two work marks over the whole work square, free every region
    // object and reset the region-id counter. Same three steps the Init-regions
    // stage opens with.
    if (side > 0)
    {
        for (int k = 0; k < side * side; ++k)
        {
            workCells_[k].data[14] = -1;                       // 0x598f24
            workCells_[k].data[15] = -1;                       // 0x598f2e
        }
    }
    for (size_t k = regions_.size(); k-- > 0; )                // 0x598f4a
        ReleaseRegionObject(regions_[k], true);
    regionIdCounter_ = 0;                                      // 0x598fbd
}

// ---------------------------------------------------------------------------
// sub_5A28C0 (0x5A28C0 - 0x5A2ECB)
// ---------------------------------------------------------------------------
void RandomMapGenerator::GrowTiberiumField(CellStruct seed, int target,
                                           int mark, bool variant)
{
    if (workCells_ == nullptr || target <= 0)
        return;

    const int overlayBase = variant ? 27 : 102;                 // 0x5a296e

    // [port-only] 群岛的矿柱由 PlaceArchipelagoOrePillars 统一放，矿田这里不撒。
    const bool archipelago = (config_.landType == RmLandType::Archipelago);

    const int capacity = 10 * target;
    std::vector<TiberiumNode> pool(static_cast<size_t>(capacity));

    std::vector<int> heap;                                      // 1-based
    heap.push_back(-1);

    const int side = size_.workSide;

    // sift-up insert (0x5a2a6d - 0x5a2ab2); the vanilla refuses a node that
    // would push count+1 onto the capacity.
    const auto push = [&](int node)
    {
        if (static_cast<int>(heap.size()) >= capacity)
            return;
        heap.push_back(node);
        int i = static_cast<int>(heap.size()) - 1;
        while (i > 1 && pool[heap[i / 2]].priority > pool[heap[i]].priority)
        {
            const int t = heap[i / 2];
            heap[i / 2] = heap[i];
            heap[i] = t;
            i /= 2;
        }
    };

    // pop the root, then sift down (sub_5AD870(1))
    const auto pop = [&]() -> int
    {
        if (heap.size() <= 1)
            return -1;
        const int root = heap[1];
        heap[1] = heap.back();
        heap.pop_back();
        int i = 1;
        const int last = static_cast<int>(heap.size()) - 1;
        for (;;)
        {
            int m = i;
            const int l = 2 * i;
            const int r = 2 * i + 1;
            if (l <= last && pool[heap[l]].priority < pool[heap[m]].priority)
                m = l;
            if (r <= last && pool[heap[r]].priority < pool[heap[m]].priority)
                m = r;
            if (m == i)
                break;
            const int t = heap[i];
            heap[i] = heap[m];
            heap[m] = t;
            i = m;
        }
        return root;
    };

    int nextNode = 0;               // v53: next free pool slot
    int placed = 0;                 // v54: cells that took ore
    int round = 0;                  // n10
    int cur = -1;                   // current pool slot, -1 = none
    bool first = false;             // v48
    CellStruct anchor{ 0, 0 };      // v52

    while (round < 10)                                          // 0x5a29a6
    {
        if (cur < 0)
        {
            // Re-seed (0x5a29bc - 0x5a2b02): clear the marks over the whole
            // diamond, push the seed with priority 0, stamp it and take it back.
            heap.clear();
            heap.push_back(-1);

            CellIterator it;
            it.Reset(cellSlots_, size_.mapWidth);
            while (MapCell* c = it.Next())                      // 0x5a29e4
            {
                const int x = c->MapCoords & 0xFFFF;
                const int y = static_cast<uint32_t>(c->MapCoords) >> 16;
                workCells_[x + side * y].data[15] = 0;          // 0x5a2a0a
            }

            nextNode = 1;
            pool[0].coords = seed;                              // 0x5a2a2f
            pool[0].priority = 0.0f;                            // 0x5a2a31
            workCells_[seed.X + side * seed.Y].data[15] = mark; // 0x5a2a56
            push(0);
            cur = pop();                                        // 0x5a2ae2
            first = false;
            ++round;
            anchor = seed;                                      // 0x5a2b02
        }

        const CellStruct cc = pool[cur].coords;
        MapCell* cell = CellAt(cc.X, cc.Y);

        // 0x5a2b39: the +69 protection mark skips the overlay / decoration work
        // for this node but still lets it feed its neighbours.
        // [移植侧] 出生点净场 10x10 跳过矿石覆盖与 TIBTRE 矿柱（但仍喂邻
        // 居，矿田会绕过净场继续生长），出生点在本阶段之前已确定。
        // [移植侧] 科技建筑的地基格同样跳过：科技建筑在本阶段之前已经放好，
        // 矿石覆盖或矿柱压到地基上会让建筑在游戏里无法使用/显示异常。
        const bool skipOverlay =
            (workCells_[cc.X + side * cc.Y].Byte(69) != 0)
            || IsOreClearArea(cc.X, cc.Y)
            || ((cell->AltFlags & AltCellFlags_ContainsBuilding) != 0);

        if (!skipOverlay)
        {
            if (!first)                                         // 0x5a2b4a
            {
                first = true;
                anchor = cc;                                    // 0x5a2b57
                heap.clear();
                heap.push_back(-1);
                nextNode = 0;

                if (!variant && !archipelago)                   // 0x5a2b88
                {
                    // Ore decoration tree (0x5a2b98 - 0x5a2c05). Vanilla builds
                    // a live object; the port records it (route B).
                    int n3;
                    do
                    {
                        n3 = F2I64(static_cast<double>(rng_.Next())
                                   * (3.0 * kUnitScale) + 1.0);
                    }
                    while (n3 > 3);
                    static const char* const kTibTreNames[3] =
                        { "TIBTRE01", "TIBTRE02", "TIBTRE03" };
                    terrainObjects_.push_back(
                        MapTerrainObject(cc, kTibTreNames[(n3 % 10) - 1]));
                }
            }

            // The cell itself (0x5a2c16 - 0x5a2c63)
            if (cell->OverlayTypeIndex == -1)
            {
                int n0xB;
                do
                {
                    n0xB = F2I64(static_cast<double>(rng_.Next())
                                 * (12.0 * kUnitScale));        // 0x5a2c38
                }
                while (n0xB > 11);
                cell->OverlayTypeIndex = overlayBase + n0xB;    // 0x5a2c4c
                ++placed;
            }
            else if (cell->OverlayData < 11)                    // 0x5a2c59
            {
                ++cell->OverlayData;
                ++placed;
            }
        }

        // Neighbours (0x5a2c67 - 0x5a2e54)
        for (int d = 0; d < 8; ++d)
        {
            const CellStruct nb{ static_cast<int16_t>(cc.X + kDirX[d]),
                                 static_cast<int16_t>(cc.Y + kDirY[d]) };
            if (!IsWithinUsableArea(nb, true))                  // 0x5a2cbc
                continue;
            MapCell* ncell = CellAt(nb.X, nb.Y);
            if (!IsPlaceholderTile(ncell))                      // 0x5a2cd9
                continue;
            // [移植侧] 矿田不把科技建筑地基格纳入扩散候选，矿石不会盖到建筑
            // 底下；地基外侧的格仍可由其他节点喂到，矿田绕着地基长。
            if ((ncell->AltFlags & AltCellFlags_ContainsBuilding) != 0)
                continue;

            bool accept = false;
            if (ncell->OverlayTypeIndex == -1)
                accept = (workCells_[nb.X + side * nb.Y].data[15] != mark);
            if (!accept && ncell->OverlayData < 11
                && ToTiberiumIdx(ncell->OverlayTypeIndex) != -1) // 0x5a2d31
                accept = true;
            if (!accept)
                continue;

            if (nextNode >= capacity)                           // pool guard
                continue;

            const double dx = static_cast<double>(anchor.X - nb.X);
            const double dy = static_cast<double>(anchor.Y - nb.Y);
            const double dist = std::sqrt(dx * dx + dy * dy);
            pool[nextNode].coords = nb;
            pool[nextNode].priority = static_cast<float>(
                static_cast<double>(rng_.Next()) * 5.0 * kUnitScale + dist);
            workCells_[nb.X + side * nb.Y].data[15] = mark;     // 0x5a2dd1
            push(nextNode);
            ++nextNode;
        }

        const int popped = pop();                               // 0x5a2e87
        if (placed < target)                                    // 0x5a2e99
        {
            cur = popped;
            continue;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// [port-only] PlaceArchipelagoOrePillars
//
// 群岛专用：矿柱（TIBTRE 装饰物）改成"每个出生点持有的数量完全相同"。
//
//   1) 目标数量 N = 1..5 随机一个；
//   2) 对每个出生点，从它出发按可走陆地（Passable / Beach）4 向泛洪找出它所在
//      的岛；同一座岛上的出生点归成一组共用一份候选格（有矿石的排前面，同档
//      按离该岛第一个出生点的距离升序）。候选格 = 不在矿石净场里、在可用区内、
//      当前没有矿柱的格子；
//   3) N 按"每岛可用格数 ÷ 该岛出生点数"取最小值兜底，保证每座岛上的每个出生
//      点都放得下 N 个（N 归零则说明有岛一个格子都放不下，此时整图不放矿柱）；
//   4) 每座岛放"该岛出生点数 × N"个矿柱，于是每个出生点持有的数量必然相同。
//
// 名称与矿田原本的取法一致（1..3 随机）；矿柱落在矿石格上时就是常见的矿树，
// 落在普通陆地格上则是裸矿柱。
// ---------------------------------------------------------------------------
void RandomMapGenerator::PlaceArchipelagoOrePillars()
{
    const int side = size_.workSide;
    const int pointCount = static_cast<int>(startingPoints_.size());
    if (workCells_ == nullptr || side <= 0 || pointCount <= 0)
        return;

    static const int16_t kDX4[4] = { 0, 1, 0, -1 };
    static const int16_t kDY4[4] = { -1, 0, 1, 0 };

    static const char* const kTibTreNames[3] =
        { "TIBTRE01", "TIBTRE02", "TIBTRE03" };

    const auto isTibTre = [](const char* n) -> bool
    {
        return n != nullptr && n[0] == 'T' && n[1] == 'I' && n[2] == 'B'
            && n[3] == 'T' && n[4] == 'R' && n[5] == 'E';
    };

    // 已经被矿柱占住的格子（含本进程早先留下的 TIBTRE）。
    std::vector<char> occupied(static_cast<size_t>(side) * side, 0);
    for (size_t i = 0; i < terrainObjects_.size(); ++i)
    {
        if (!isTibTre(terrainObjects_[i].typeName))
            continue;
        const CellStruct c = terrainObjects_[i].coords;
        if (c.X >= 0 && c.Y >= 0 && c.X < side && c.Y < side)
            occupied[static_cast<size_t>(c.X) + static_cast<size_t>(side) * c.Y] = 1;
    }

    // ---- 逐岛归并：同一座岛上的出生点共用一份候选格 -------------------------
    struct IslandSlot
    {
        int                     repIdx;      // 岛的极小索引，用来判同一座岛
        std::vector<int>        points;      // 这座岛上的出生点序号
        std::vector<CellStruct> oreCells;    // 岛上有矿石的可用格
        std::vector<CellStruct> plainCells;  // 岛上普通可用格
        IslandSlot() : repIdx(0) {}
    };
    std::vector<IslandSlot> islands;

    std::vector<int> seen(static_cast<size_t>(side) * side, 0);
    int stamp = 0;
    std::vector<int> stack;
    std::vector<CellStruct> islandCells;

    for (int p = 0; p < pointCount; ++p)
    {
        const CellStruct sp = startingPoints_[static_cast<size_t>(p)].coords;
        if (sp.X < 0 || sp.Y < 0 || sp.X >= side || sp.Y >= side)
        {
            // 点的坐标不合法就没法保证数量一致，整图不放。
            DiagLog("ORE-PILLAR archipelago: 出生点坐标非法，整图不放矿柱");
            return;
        }

        // 该出生点所在岛的整片可走陆地。
        ++stamp;
        islandCells.clear();
        stack.clear();
        const int spIdx = sp.X + side * sp.Y;
        int repIdx = spIdx;
        seen[static_cast<size_t>(spIdx)] = stamp;
        stack.push_back(spIdx);
        while (!stack.empty())
        {
            const int idx = stack.back();
            stack.pop_back();
            if (idx < repIdx)
                repIdx = idx;
            const int px = idx % side;
            const int py = idx / side;
            islandCells.push_back(
                CellStruct{ static_cast<int16_t>(px), static_cast<int16_t>(py) });
            for (int d = 0; d < 4; ++d)
            {
                const int nx = px + kDX4[d];
                const int ny = py + kDY4[d];
                if (nx < 0 || ny < 0 || nx >= side || ny >= side)
                    continue;
                const int ni = nx + side * ny;
                if (seen[static_cast<size_t>(ni)] == stamp)
                    continue;
                if (!CellExists(static_cast<int16_t>(nx), static_cast<int16_t>(ny)))
                    continue;
                const MapCell* c = CellAt(nx, ny);
                if (c->Passability != PassabilityType_Passable
                    && c->Passability != PassabilityType_Beach)
                    continue;
                seen[static_cast<size_t>(ni)] = stamp;
                stack.push_back(ni);
            }
        }

        // 找同一座岛（代表格相同）的已有分组。
        int isl = -1;
        for (size_t k = 0; k < islands.size(); ++k)
        {
            if (islands[k].repIdx == repIdx)
            {
                isl = static_cast<int>(k);
                break;
            }
        }
        if (isl < 0)
        {
            islands.push_back(IslandSlot());
            isl = static_cast<int>(islands.size()) - 1;
            islands[static_cast<size_t>(isl)].repIdx = repIdx;
        }
        IslandSlot& slot = islands[static_cast<size_t>(isl)];
        const bool firstOnIsland = slot.points.empty();
        slot.points.push_back(p);

        // 候选格每座岛只收集一次：不在矿石净场里、在可用区内、当前没有矿柱。
        if (firstOnIsland)
        {
            for (size_t i = 0; i < islandCells.size(); ++i)
            {
                const CellStruct c = islandCells[i];
                if (occupied[static_cast<size_t>(c.X)
                             + static_cast<size_t>(side) * c.Y])
                    continue;
                if (IsOreClearArea(c.X, c.Y))
                    continue;
                if (!IsWithinUsableArea(c, true))
                    continue;
                const MapCell* cell = CellAt(c.X, c.Y);
                // 科技建筑地基格不能在候选里：写盘前的剪枝会删掉地基上的任何
                // 地形对象，若这里允许选中就会让该出生点少一个矿柱。
                if ((cell->AltFlags & AltCellFlags_ContainsBuilding) != 0)
                    continue;
                if (ToTiberiumIdx(cell->OverlayTypeIndex) != -1)
                    slot.oreCells.push_back(c);
                else
                    slot.plainCells.push_back(c);
            }

            // 有矿石的排前面；同档按到本岛第一个出生点的距离升序。
            const auto nearFirst =
                [&](const CellStruct& a, const CellStruct& b) -> bool
            {
                const int ax = a.X - sp.X, ay = a.Y - sp.Y;
                const int bx = b.X - sp.X, by = b.Y - sp.Y;
                return ax * ax + ay * ay < bx * bx + by * by;
            };
            std::sort(slot.oreCells.begin(), slot.oreCells.end(), nearFirst);
            std::sort(slot.plainCells.begin(), slot.plainCells.end(), nearFirst);
        }
    }

    // ---- 统一数量：1..5 随机，再按"每岛可用格数 ÷ 该岛点数"兜底 ------------
    int target = rng_.RandomFloatRange(1, 5);
    for (size_t k = 0; k < islands.size(); ++k)
    {
        const int pts = static_cast<int>(islands[k].points.size());
        const int avail = static_cast<int>(islands[k].oreCells.size()
                                         + islands[k].plainCells.size());
        const int cap = (pts > 0) ? avail / pts : 0;
        if (cap < target)
            target = cap;
    }
    if (target <= 0)
    {
        DiagLog("ORE-PILLAR archipelago: 有出生点所在岛没有可用格，整图不放矿柱");
        return;
    }

    // ---- 每座岛放"该岛点数 × target"个，各点数量因此必然相同 ----------------
    for (size_t k = 0; k < islands.size(); ++k)
    {
        IslandSlot& slot = islands[k];
        const int need = static_cast<int>(slot.points.size()) * target;

        int placed = 0;
        for (size_t i = 0; i < slot.oreCells.size() && placed < need; ++i)
        {
            const CellStruct c = slot.oreCells[i];
            const size_t ci = static_cast<size_t>(c.X)
                            + static_cast<size_t>(side) * c.Y;
            if (occupied[ci])
                continue;
            occupied[ci] = 1;
            const int n3 = rng_.RandomFloatRange(1, 3);
            terrainObjects_.push_back(
                MapTerrainObject(c, kTibTreNames[n3 - 1]));
            ++placed;
        }
        for (size_t i = 0; i < slot.plainCells.size() && placed < need; ++i)
        {
            const CellStruct c = slot.plainCells[i];
            const size_t ci = static_cast<size_t>(c.X)
                            + static_cast<size_t>(side) * c.Y;
            if (occupied[ci])
                continue;
            occupied[ci] = 1;
            const int n3 = rng_.RandomFloatRange(1, 3);
            terrainObjects_.push_back(
                MapTerrainObject(c, kTibTreNames[n3 - 1]));
            ++placed;
        }
        if (placed < need)
        {
            DiagLog("ORE-PILLAR archipelago: 岛%d 只放下 %d/%d 个矿柱",
                    static_cast<int>(k), placed, need);
        }
    }

    DiagLog("ORE-PILLAR archipelago: 每个出生点 %d 个矿柱，共 %d 个点 %d 座岛",
            target, pointCount, static_cast<int>(islands.size()));
}
