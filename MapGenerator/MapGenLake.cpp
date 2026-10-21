// ============================================================================
// MapGenLake.cpp - lake subsystem of the random map generator
//
//   GenerateLake            sub_59C920   standalone lake generation
//   ClearPreviousGeneration sub_5A0410   ring-spread relabel
//
// Two call sites share GenerateLake: the river exit phase (0x59e22c, seeded
// with the river-mouth cell on natural termination) and the top-level lake
// stage of GenerateSpecialTerrain (0x59c5fb, random start points, up to 10
// tries).
//
// Full annotated walkthrough: sub_59C580_注释.md §6.1 - §6.7.
// ============================================================================

#include "pch.h"
#include "MapGen.h"
#include "MapGenFastSqrt.h"

#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Direction offsets, Neighbours table order (0x89F688) - the same order as
// CellClass::GetNeighbourCell. GenerateLake walks only the four orthogonal
// steps 0 / 2 / 4 / 6 (0x59cef4: the direction index steps by 2).
// ---------------------------------------------------------------------------
static const int16_t kLakeDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int16_t kLakeDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

namespace
{
// Priority-queue node (vanilla Block, allocated at 0x59c98e): 8 bytes - the
// packed cell coordinates plus the float priority at +4 (0x59cfd0 / 0x59d053).
// Nodes are allocated in order and never recycled; the heap stores pointers
// into the node vector, which is therefore reserved once up front and never
// allowed to outgrow that reservation.
struct LakeNode
{
    CellStruct coords;     // +0  (low16 X, high16 Y)
    float      priority;   // +4
};

// 1-based min-heap over LakeNode* (vanilla heap object, 0x14 bytes:
// { count, capacity, slots, maxSeen, minSeen }; the two "seen" pointers are
// dead stores in the vanilla code and are dropped here).
struct LakeHeap
{
    std::vector<LakeNode*> slots;    // slots[0] unused; slots[1..count] live
    size_t count = 0;

    // Sift-down (sub_5AD870): restore the heap order at `a2` after the root
    // was overwritten by the last slot. The children of node i are 2i / 2i+1;
    // the smaller-priority child bubbles up until both children are larger
    // (or absent). Index 0 is unused, so 1-based slots line up directly.
    void SiftDown(size_t a2)
    {
        size_t v2 = a2;
        size_t v3 = 2 * a2;
        const size_t v4 = 2 * a2 + 1;
        // No left child, or the parent already wins -> stay at a2.
        if (!(2 * a2 <= count) ||
            slots[a2]->priority <= slots[2 * a2]->priority)
            v3 = a2;
        // The right child is smaller still -> pick it.
        if (v4 <= count && slots[v3]->priority > slots[v4]->priority)
            v3 = 2 * a2 + 1;
        if (v3 == a2)
            return;
        do
        {
            const size_t v6 = 2 * v3 + 1;
            LakeNode* v7 = slots[v2];
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

    // Sift-up insert (vanilla inline: seed push 0x59cdf0, neighbour push
    // 0x59d082). Silently dropped once count + 1 would reach the capacity.
    void Push(LakeNode* node)
    {
        size_t v = count + 1;
        size_t parent = v >> 1;
        if (v >= slots.size())
            return;
        while (v > 1 && slots[parent]->priority > node->priority)
        {
            slots[v] = slots[parent];
            v = parent;
            parent >>= 1;
        }
        slots[v] = node;
        count = v;
    }

    // Pop the minimum-priority node (vanilla inline: swap root with the last
    // slot, shrink, then sub_5AD870).
    LakeNode* Pop()
    {
        if (count == 0)
            return nullptr;
        LakeNode* root = slots[1];
        slots[1]     = slots[count];
        slots[count] = nullptr;
        --count;
        SiftDown(1);
        return root;
    }
};
}  // namespace

// ---------------------------------------------------------------------------
// ClearPreviousGeneration - sub_5A0410 - ring-spread relabel.
//
// Called once by the lake auto-seed pass as ClearPreviousGeneration(0, 2, -2)
// (0x59ca9a). Takes ring 0 of the body carrying `genCode` (BuildWaterRing,
// sub_5A0700) and then walks `rings` successive rings:
//   (1) every cell of the current ring has its generation mark stamped with
//       `mark` (data[14]) and its tile reset to bare land at baseLevel_;
//   (2) the next ring is the set of in-diamond 8-neighbours of the current
//       ring that still carry the generation mark and were not claimed by this
//       round - their visited byte (data[15]) holds the round number r + 1.
// The visited byte is cleared over the whole diamond before and after the walk
// when more than one ring is requested.
//
// Net effect for the lake caller: the code-0 body's boundary and a `mark`-wide
// band behind it are relabelled, so the qualification sweep that follows turns
// every mark cell (== -2) back into free land, wiping the previous generation's
// traces.
//
// Vanilla returns 1; the call site ignores the value, so this stays void.
// ---------------------------------------------------------------------------
void RandomMapGenerator::ClearPreviousGeneration(int genCode, int rings,
                                                 int mark)
{
    std::vector<CellStruct> ring = BuildWaterRing(genCode);   // 0x5a0422

    const bool clearVisited = (rings > 1);                    // 0x5a042d
    if (clearVisited)
    {
        CellIterator it;                                      // 0x5a043b
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            const int x = cell->MapCoords & 0xFFFF;
            const int y = (uint32_t)cell->MapCoords >> 16;
            WorkAt(x, y).data[15] = 0;                        // byte 60
        }
    }

    for (int r = 0; r < rings; ++r)
    {
        // (1) Stamp the current ring (vanilla walks it back to front).
        for (int j = (int)ring.size() - 1; j >= 0; --j)       // 0x5a0499
        {
            const CellStruct c = ring[j];
            WorkAt(c.X, c.Y).data[14] = mark;                 // 0x5a04ca
            MapCell* cell = CellAt(c.X, c.Y);
            cell->IsoTileTypeIndex = 0;                       // 0x5a04dd
            cell->Height = 0;                                 // 0x5a04e0
            cell->Level = baseLevel_;                         // 0x5a04ee
        }

        // (2) Grow the next ring, for every round but the last.
        if (r < rings - 1)                                    // 0x5a0501
        {
            std::vector<CellStruct> next;
            for (int j = (int)ring.size() - 1; j >= 0; --j)   // 0x5a0540
            {
                const CellStruct c = ring[j];
                for (int dir = 0; dir < 8; ++dir)             // 0x5a055c
                {
                    const int16_t nx =
                        static_cast<int16_t>(c.X + kLakeDirX[dir]);
                    const int16_t ny =
                        static_cast<int16_t>(c.Y + kLakeDirY[dir]);

                    if (!CellExists(nx, ny))                  // 0x5a05b9
                        continue;

                    WorkCell& nw = WorkAt(nx, ny);
                    if (nw.data[14] != genCode)               // 0x5a05d6
                        continue;
                    if (nw.data[15] == r + 1)                 // 0x5a05e7
                        continue;                             // claimed already

                    next.push_back(CellStruct{ nx, ny });     // 0x5a0625
                    nw.data[15] = static_cast<unsigned char>(r + 1);  // 0x5a0628
                }
            }
            ring.swap(next);
        }
    }

    if (clearVisited)                                         // 0x5a0696
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            const int x = cell->MapCoords & 0xFFFF;
            const int y = (uint32_t)cell->MapCoords >> 16;
            WorkAt(x, y).data[15] = 0;                        // byte 60
        }
    }
}

// ---------------------------------------------------------------------------
// GenerateLake - sub_59C920 - one standalone lake attempt.
//
// Phases (doc §6.1 - §6.7), with the vanilla addresses they mirror:
//   1  budget gate         0x59c92e - 0x59c970
//   2  node block + heap   0x59c976 - 0x59ca07
//   3  qualification       0x59ca10 - 0x59cc83   (+ auto-mode seed sampling)
//   4  area sampling       0x59cca7 - 0x59cd76
//   5  seed push + growth  0x59cd95 - 0x59d147
//   6  heap drain + gates  0x59d14d - 0x59d259
//   7  finish / rollback   0x59d25e - 0x59d418
//
//   startXY     packed seed cell (low16 X, high16 Y); (0,0) = automatic mode
//               (the routine samples and qualifies its own start point)
// Returns true on success (the grown cell count is added to the water-cell
// budget); a failure rolls the stamped cells back to plain land.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::GenerateLake(int* startXY)
{
    const int genCode = genCode_;
    const bool autoMode = (*startXY == 0);

    // ---- 1. Budget gate (0x59c92e - 0x59c970) ----------------------------
    // budget = F2I64(H * W * waterAmount * 0.008 + 100). With waterAmount 0 the
    // integer product stays 0 and the gate always fails (no water -> no lake).
    // remaining = budget - usedWaterCells_; 75 or less gives up.
    int budget = waterAmount_;
    if (waterAmount_ != 0)
        budget = F2I64((double)(size_.height * size_.width * waterAmount_)
                       * 0.008 + 100.0);
    const int remaining = budget - usedWaterCells_;
    if (remaining <= 75)                                  // 0x59c970
        return false;

    bool ok = true;                                       // v75

    // [TEMP DIAG] lake attempt that passed the budget gate.
    DiagLog("LAKE g=%d auto=%d remaining=%d",
            genCode, static_cast<int>(autoMode), remaining);

    // ---- 2. Node block + min-heap (0x59c976 - 0x59ca07) ------------------
    int cap = 2 * remaining + 2;                          // 0x59c976
    if (cap <= 100)
        cap = 100;                                        // 0x59c981

    std::vector<LakeNode> nodes;
    nodes.reserve(static_cast<size_t>(cap));              // vanilla Block
    LakeHeap heap;
    heap.slots.assign(static_cast<size_t>(cap) + 1, nullptr);

    // ---- 3a. Clear the enqueue mark over the diamond (0x59ca21) ----------
    // Iterator order; byte 60 = WorkCell data[15].
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            const int x = cell->MapCoords & 0xFFFF;
            const int y = (uint32_t)cell->MapCoords >> 16;
            WorkAt(x, y).data[15] = 0;
        }
    }

    // ---- 3b. Qualification (0x59ca7b - 0x59cadc) -------------------------
    // Linear sweep of the whole work square (side * side cells). Byte 68 =
    // WorkCell data[17] is the "may host lake water" flag.
    const int side = size_.workSide;
    const int workCount = side * side;                    // 0x59ca61

    if (!autoMode)
    {
        // Specified seed (river mouth): every cell may host the lake.
        for (int i = 0; i < workCount; ++i)               // 0x59cc89
            WorkAtLinear(i).data[17] = 1;
    }
    else
    {
        for (int i = 0; i < workCount; ++i)               // 0x59ca7b
            WorkAtLinear(i).data[17] = 0;

        ClearPreviousGeneration(0, 2, -2);                // 0x59ca9a sub_5A0410

        for (int i = 0; i < workCount; ++i)               // 0x59caaa
        {
            WorkCell& w = WorkAtLinear(i);
            const int mark = w.data[14];                  // byte 56
            if (mark == 0 || mark == genCode)
                w.data[17] = 1;                           // free, or ours
            else if (mark == -2)
                w.data[14] = 0;                           // drop stale marker
        }
    }

    // ---- 3c. Auto-mode seed sampling (0x59caf1 - 0x59cc83) ---------------
    // x = rand % W', y = rand % H' (rejection sampling), remapped onto the
    // diamond as (x + y + 1, W' - x + y). Accept only a free, placeholder,
    // qualified cell; 200 tries and the attempt fails.
    int seedPacked;                                       // pMapCoord_
    if (autoMode)
    {
        const int Wp = size_.mapWidth;                    // W'
        const int Hp = size_.mapHeight;                   // H'
        int tries = 0;                                    // n200
        for (;;)
        {
            int x;
            do                                            // 0x59cb09
            {
                x = F2I64((double)(uint32_t)rng_.Next() * Wp * kUnitScale);
            }
            while (x > Wp - 1);

            int y;
            do                                            // 0x59cb4a
            {
                y = F2I64((double)(uint32_t)rng_.Next() * Hp * kUnitScale);
            }
            while (y > Hp - 1);

            const int16_t sx = static_cast<int16_t>(x + y + 1);      // 0x59cbc9
            const int16_t sy = static_cast<int16_t>(Wp - x + y);     // 0x59cbcb
            seedPacked = (int)sx | ((int)sy << 16);

            if (++tries >= 200)                           // 0x59cbee
                return false;

            WorkCell& w = WorkAt(sx, sy);
            if (w.data[14] != 0)                          // 0x59cc17
                continue;
            MapCell* cell = CellAt(sx, sy);
            if (!IsPlaceholderTile(cell))                 // 0x59cc34
                continue;
            if (w.data[17] != 0)                          // 0x59cc60
                break;                                    // -> LABEL_50
        }
    }
    else
    {
        seedPacked = *startXY;                            // 0x59cca3
    }

    // [TEMP DIAG] resolved lake seed cell.
    DiagLog("LAKE-SEED g=%d (%d,%d)",
            genCode, seedPacked & 0xFFFF,
            static_cast<uint32_t>(seedPacked) >> 16);

    // ---- 4. Area sampling (0x59cca7 - 0x59cd76) --------------------------
    // lower = max(76, remaining); sigma = remaining / 6 (integer division),
    // mu = remaining / 3. If the mu +/- sigma window leaves [75, lower],
    // recentre sigma on the 75 floor. Gaussian rejection keeps the drawn area
    // inside [75, lower]; target = F2I64(area).
    int lower = remaining;                                // n76_1
    if (lower < 76)                                       // 0x59ccbc
        lower = 76;
    double sigma = (double)(remaining / 6);               // 0x59cce7 (v88)
    double mu    = (double)(remaining / 3);               // 0x59ccfa (v87)
    if (mu - sigma > (double)lower || mu + sigma < 75.0)  // 0x59cd20
    {
        sigma = ((double)lower - 75.0) * 0.5;             // 0x59cd32
        mu    = sigma + 75.0;                             // 0x59cd3c
    }
    double area;
    do                                                    // 0x59cd70
    {
        do                                                // 0x59cd61
        {
            area = rng_.Gaussian() * sigma + mu;          // sub_5980C0
        }
        while (area < 75.0);
    }
    while (area > (double)lower);
    const int target = F2I64(area);                       // n76 (0x59cd76)

    // ---- 5. Seed push + growth (0x59cd95 - 0x59d147) ---------------------
    // Reset the heap, plant the seed as node 0, push it and pop it straight
    // back (0x59cd9f - 0x59ce64): the seed becomes the first current cell.
    heap.count = 0;
    int expanded = 0;                                     // n76_4
    int seedX = seedPacked & 0xFFFF;                      // sign-agnostic low16
    int seedY = (uint32_t)seedPacked >> 16;
    if (seedX >= 0x8000) seedX -= 0x10000;
    if (seedY >= 0x8000) seedY -= 0x10000;

    nodes.emplace_back();                                 // vanilla Block[0]
    LakeNode* seedNode = &nodes.back();
    seedNode->coords.X = static_cast<int16_t>(seedX);
    seedNode->coords.Y = static_cast<int16_t>(seedY);
    seedNode->priority = 0.0f;                            // 0x59cda9
    WorkAt(seedX, seedY).data[15] = genCode;              // 0x59cdd4 (occupies)

    heap.Push(seedNode);                                  // 0x59cddd - 0x59ce18
    LakeNode* current = heap.Pop();                       // 0x59ce32 - 0x59ce64

    if (target > 0)                                       // 0x59ce6a
    {
        while (current != nullptr && ok)                  // 0x59ce84
        {
            const int cx = current->coords.X;
            const int cy = current->coords.Y;

            // (1) Stamp the current cell as water of this generation.
            WorkAt(cx, cy).data[14] = genCode;            // 0x59ceba
            MapCell* cell = CellAt(cx, cy);
            cell->IsoTileTypeIndex = waterTileIndex_;     // 0x59ced3 (nIdx)
            cell->Height = 0;                             // 0x59ced6

            // (2) Look at the four orthogonal neighbours.
            for (int step = 0; step < 8; step += 2)       // 0x59cef4 - 0x59d0f8
            {
                const int dir = step & 7;
                const int16_t nx = static_cast<int16_t>(cx + kLakeDirX[dir]);
                const int16_t ny = static_cast<int16_t>(cy + kLakeDirY[dir]);

                if (!CellExists(nx, ny))                  // 0x59cf60 (diamond)
                    continue;

                WorkCell& nw = WorkAt(nx, ny);
                MapCell*  nCell = CellAt(nx, ny);

                if (nw.data[14] != 0 ||                   // taken tile
                    nw.data[15] == genCode ||             // already enqueued
                    !IsPlaceholderTile(nCell) ||          // not a free tile
                    nw.data[17] == 0)                     // not qualified
                {
                    // Foreign generation code is fatal; anything else is just
                    // skipped (0x59d0d2 - 0x59d0e5).
                    const int mark = nw.data[14];
                    if (mark != 0 && mark != genCode)     // 0x59d0e3
                        ok = false;
                    continue;
                }

                // (3) Accept: allocate a node and push it. Priority draws one
                // RNG value either way (the draw happens before the capacity
                // test, 0x59d010 - 0x59d053):
                //   priority = 10 * rand * kUnitScale
                //            + 0.5 * dist(neighbour, seed)
                //            - 0.02 * expanded
                // 距离项原版用的是 YRMath::sqrt（0x4cac40 位运算查表快速近似，
                // 返回 float，最大相对误差约 0.003%），不是精确平方根。先前移植
                // 误用 std::sqrt(double)，距离项的细微偏差在随机项接近时翻转
                // 小顶堆在相邻前沿格之间的选择，湖形因此长出原版没有的单格水
                // 刺/陆尖角（后续铺岸片无法覆盖）。这里按原版逐位复刻。
                const int dx = seedX - nx;                // 0x59cfda
                const int dy = seedY - ny;                // 0x59cfe1
                const int d2 = dx * dx + dy * dy;
                const double randUnit =
                    (double)(uint32_t)rng_.Next() * kUnitScale;
                const float dist = MapGenFastSqrt::Sqrt((double)d2);  // 0x59cff3
                // 原版 x87 链：fild 随机数 -> fmul kUnitScale(double) -> fld 10.0
                // -> 距离 fmul 0.5(float) -> expanded fild -> fmul 0.02(float)
                // -> fstp dword。中间在 80 位寄存器，最后落 float；SSE2 下全程
                // double 再转 float 与该结果只差末位，故字面量按原版取 float。
                const float priority = static_cast<float>(
                    10.0 * randUnit
                    + 0.5 * static_cast<double>(dist)
                    - static_cast<double>(0.02f) * static_cast<double>(expanded));

                nw.data[15] = genCode;                    // 0x59d05c (occupies)
                if (nodes.size() < (size_t)cap)
                {
                    nodes.emplace_back();
                    LakeNode* node = &nodes.back();
                    node->coords.X = nx;
                    node->coords.Y = ny;
                    node->priority = priority;
                    heap.Push(node);                      // 0x59d082 sift-up
                }
                // else: vanilla overruns its fixed block; the node is dropped.
            }

            // (4) Advance: count the cell and pop the next candidate.
            ++expanded;                                   // 0x59d103
            current = heap.Pop();
            if (expanded >= target)                       // 0x59d147
                break;
        }
    }

    // ---- 6. Heap drain (0x59d14d - 0x59d23f) -----------------------------
    // Leftover candidates are stamped if still free + qualified; an
    // unacceptable cell only clears the success flag (no neighbours added).
    {
        LakeNode* node = heap.Pop();
        while (node != nullptr && ok)
        {
            const int nx = node->coords.X;
            const int ny = node->coords.Y;
            WorkCell& w = WorkAt(nx, ny);
            MapCell*  c = CellAt(nx, ny);
            if (w.data[14] == 0 && IsPlaceholderTile(c) && w.data[17] != 0)
            {
                c->IsoTileTypeIndex = waterTileIndex_;    // 0x59d1de
                c->Height = 0;                            // 0x59d1e1
                w.data[14] = genCode;                     // 0x59d1ee
            }
            else
            {
                ok = false;                               // 0x59d1f3
            }
            ++expanded;                                   // 0x59d22f
            node = heap.Pop();
        }
    }

    // ---- 6b. Size gate (0x59d23f - 0x59d259) -----------------------------
    if (ok && (expanded <= 75 || expanded <= target / 4))
        ok = false;

    // ---- 7a. Finish (0x59d25e - 0x59d3ac) --------------------------------
    // Auto mode only: smooth, then lakeshore-expand, then stamp the green
    // ground tile over the cells the expansion left as placeholders. A
    // specified-seed lake (river exit) skips all of it - the river caller
    // post-processes.
    if (ok && autoMode)
    {
        if (SmoothWaterBody(genCode, 0))                  // 0x59d289 sub_57A0C0
        {
            if (ExpandWaterBody(genCode, 1, 0, 0, 512, 512, 0, 0))  // 0x59d32a
            {
                CellIterator it;
                it.Reset(cellSlots_, size_.mapWidth, slotStride_);
                while (MapCell* c = it.Next())
                {
                    WorkCell& w = WorkAt(c->MapCoords & 0xFFFF,
                                         (uint32_t)c->MapCoords >> 16);
                    if (w.data[14] != genCode)            // 0x59d384
                        continue;
                    const int tile = c->IsoTileTypeIndex; // 0x59d386
                    if (tile == 0 || tile == 0xFFFF)      // 0x59d393
                        c->IsoTileTypeIndex = greenTileIndex_;  // 0x59d39b (dword_AA0E18)
                }
            }
            else
            {
                ok = false;
            }
        }
        else
        {
            ok = false;
        }
    }

    // ---- 7b. Result (0x59d2c3 - 0x59d2e3) --------------------------------
    if (ok)
    {
        usedWaterCells_ += expanded;                      // 0x59d2d5
        DiagLog("LAKE-OK g=%d expanded=%d", genCode, expanded);
        return true;                                      // 0x59d2e3
    }

    // ---- 7c. Rollback (0x59d3b6 - 0x59d418) ------------------------------
    // Undo every cell this attempt stamped, back to plain land.
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* c = it.Next())
        {
            WorkCell& w = WorkAt(c->MapCoords & 0xFFFF,
                                 (uint32_t)c->MapCoords >> 16);
            if (w.data[14] != genCode)                    // 0x59d3fc
                continue;
            w.data[14] = 0;                               // 0x59d401
            w.Byte(75) = 0;                               // 0x59d404
            c->IsoTileTypeIndex = 0;                      // 0x59d408
            c->Height = 0;                                // 0x59d40b
            c->Level = baseLevel_;                        // 0x59d418
        }
    }
    return false;
}
