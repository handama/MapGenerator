// ============================================================================
// MapGenLAT.cpp - the "RMG: Creating LATs, rocks etc" stage
// (sub_598960 @ 0x599215 - 0x599353).
//
// The stage slice is four calls and one inline loop, with no land-type gate and
// no RNG of its own:
//
//     0x599215  push "RMG: Creating LATs, rocks etc\n", nullsub_1    debug
//     0x59922B  sub_5A38C0(this)          // 1. the per-cell density fields
//     0x599232  sub_5A3AE0(this)          // 2. LAT blobs + SetupLAT + tree
//                                         //    patches + the overlay tail
//     0x59923E  the inline per-cell loop  // 3. the uniform density overwrite
//     0x5992AB  sub_5A4280(this)          // 4. rocks + tree patches
//     0x5992B9  ... UI / session tail (NOT ported): psub_48D1D0, sub_69AE90,
//               sub_643C50, sub_641140, SendMessageA(WM_PAINT), sub_5E7EB0
//     0x599354  "RMG: Recalculating cell attributes"            next stage
//
// Full annotated walkthrough: 599215_CreatingLATs.c, 5A38C0.c, 5A3AE0.c,
// 5A4280.c and 5A4B60_5A45E0.c in the decompile folder.
//
// ----------------------------------------------------------------------------
// THE STAGE BODY IS A FOUR-STEP SEQUENCE PLUS AN INLINE LOOP
// ----------------------------------------------------------------------------
//   CreateLATs below mirrors the instruction order one call per step:
//
//     1. PrepareLATDensities()  sub_5A38C0 - writes the three density doubles
//        (work +0x18 / +0x20 / +0x28) from this[23] * 0.01, but only for shore
//        cells: its `else if (work.double(+0x28) != 0)` branch requires a
//        non-zero field which nothing has written yet at this point (WorkCell's
//        constructor zeroes it), so for the RMG every non-shore cell keeps
//        0 / 0 / 0. See 5A38C0.c.
//
//     2. ScatterLATBlobs()      sub_5A3AE0 - dresses the cells that carry no
//        own tile with LAT tiles (the three-way branch on the densities:
//        GreenTile / SandTile / RoughTile), then refreshes SetupLAT on every
//        cell, then plants tree patches, then writes overlay 168..177 on a
//        random subset and recalculates each.
//
//     3. ResetLATDensityFields() the inline loop at 0x59923E - writes the SAME
//        two doubles into EVERY work cell:
//            work + 0x20 = 0.001        (dword pair 0x7ED7F8 / 0x7ED7FC)
//            work + 0x28 = 0.005        (dword pair 0x7ED7F0 / 0x7ED7F4)
//        This overwrites whatever step 1 left and arms step 4, which tests
//        work.double(+0x28) as the rock probability.
//
//     4. ScatterRocks()         sub_5A4280 - places a rock blob on every flat,
//        unprotected placeholder cell with probability work.double(+0x28)
//        (= 0.005 after step 3), refreshes SetupLAT again, then runs the same
//        tree-patch loop as step 2.
//
//   Both patch loops share the same budget:
//       F2I64( (this[25] * 0.1 + 0.7) * (this[23] * 0.01) * this[191] )
//   with this[25] = MapGenConfig::sizeSlider (0..3), this[23] =
//   GlobalMapOptions::vegetation (60..100 with the shipped rmgmd.ini) and
//   this[191] = RMGSettings::MaxTrees (600) - so the budget is 252..600 and the
//   loop's own 100-patch cap always wins.
//
// ----------------------------------------------------------------------------
// THE WORK-CELL FIELDS THIS STAGE USES
// ----------------------------------------------------------------------------
//   work + 0x18  double  0.02 * half (shore) - written by step 1, read by the
//                        LAT branch as "the GreenTile probability"
//   work + 0x20  double  0.0 (shore) / 0.001 (step 3) - the SandTile test
//   work + 0x28  double  0.005 - the RoughTile test and the rock probability
//   work dword +0x3C (60) the access/occupancy mark: cleared for every cell at
//                        the head of steps 2 and 4, then set to the blob's seed
//                        slot index by PlaceBlob
//   work dword +0x44 (68) data[17] - not touched here
//   work byte +0x45 (69) the hill-protection mark (read only)
//   work byte +0x47 (71) "a tree has been planted here" - NEW in this stage,
//                        set by ScatterTrees and tested by it
//
// ----------------------------------------------------------------------------
// SUBORDINATE STATUS
// ----------------------------------------------------------------------------
//   implemented : every step -
//                 PrepareLATDensities   sub_5A38C0
//                 ScatterLATBlobs       sub_5A3AE0
//                 ResetLATDensityFields the inline 0x59923E loop
//                 ScatterRocks          sub_5A4280
//                 PlaceBlob             sub_5A4B60
//                 ScatterTrees          sub_5A45E0
//                 PlaceGreenPatches     the loop both scatterers inline
//   stub        : none
//
//   Everything they need already exists in the port:
//     IsPlaceholderTile (sub_486380), SetupLAT (sub_47CA80),
//     IsPaveTile / IsMiscPaveTile (sub_486670 / sub_486650),
//     the green-ground predicate (sub_4867B0) and the ClearToSandLat family
//     test (sub_486790 = [clearToSandLatIndex_, +16)),
//     the tile bases greenTileIndex_ / sandTileIndex_ / roughTileIndex_,
//     the RNG (rng_ = dword_ABE890 for Random / Gaussian, globalRng_ for the
//     options), RandomRanged (sub_598030), the attribute recalc
//     (RecalcAttributes = sub_47D2B0), and MapTerrainObject + terrainObjects_
//     for the tree objects (route B - the vanilla creates real ObjectClass
//     instances, which the port does not have).
// ============================================================================

#include "pch.h"
#include "MapGen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// The stage body - the four calls of sub_598960's slice, in order.
// ---------------------------------------------------------------------------
void RandomMapGenerator::CreateLATs()
{
    PrepareLATDensities();        // 0x59922B  sub_5A38C0
    ScatterLATBlobs();            // 0x599232  sub_5A3AE0
    ResetLATDensityFields();      // 0x59923E  the inline loop
    ScatterRocks();               // 0x5992AB  sub_5A4280
}

// ---------------------------------------------------------------------------
// Step 1 - PrepareLATDensities (sub_5A38C0, 0x5A38C0 - 0x5A3A50).
//
//     half = this[23] * 0.01;                    // the vegetation density
//     for every diamond cell:
//         if (IsShoreTile(cell))                 // sub_4865B0
//             // the vanilla walks the 5x5 block around the cell but stores
//             // through the SAME work pointer, so it is one write:
//             work.double(+0x28) = 0.005;
//             work.double(+0x20) = 0.0;
//             work.double(+0x18) = 0.02 * half * 10.0;
//         else if (work.double(+0x28) == 0.0)
//             work.double(+0x28) = 0.005;
//             work.double(+0x18) = 0.02 * half;
//             work.double(+0x20) = 0.005;
//
// The else condition is `== 0.0`, read straight off the disassembly: 0x5A39F8
// compares +0x28 against dbl_7E2800 (measured 0.0), 0x5A3A00 tests AH bit 6
// (C3) and 0x5A3A03 `jz`es past the body when C3 is clear - so the body runs
// when the two are EQUAL. Nothing has written +0x28 before this stage and
// WorkCell's constructor zeroes it, so on a fresh map EVERY non-shore cell
// takes the branch; that is what gives the LAT scatter its probabilities.
//
// No RNG.
// ---------------------------------------------------------------------------
void RandomMapGenerator::PrepareLATDensities()
{
    if (cellSlots_ == nullptr || workCells_ == nullptr)
        return;

    // this[23] - the vegetation density rolled by sub_597260 into
    // GlobalMapOptions::vegetation (60..100 with the shipped rmgmd.ini).
    const double half = static_cast<double>(globalOptions_.vegetation) * 0.01;
    const int side = size_.workSide;

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);                  // 0x5a38d4
    while (MapCell* cell = it.Next())
    {
        const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t y = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);
        WorkCell& w = workCells_[x + side * y];            // 0x5a3920

        // The three density doubles are at +0x18, +0x20 and +0x28 (pinned by
        // the disassembly - Hex-Rays renders them through mixed dword and qword
        // accesses, which reads like +0x0C / +0x10 / +0x14).
        double* const density18 = reinterpret_cast<double*>(w.data + 6);
        double* const density20 = reinterpret_cast<double*>(w.data + 8);
        double* const density28 = reinterpret_cast<double*>(w.data + 10);

        if (IsShoreTile(cell))                             // 0x5a3922 sub_4865B0
        {
            // The vanilla walks the 5x5 block around the cell
            // (0x5a3938 - 0x5a39e0) but stores through the SAME `work` pointer
            // every iteration - the base never changes - and the block always
            // contains the cell itself, which is inside the diamond. So the
            // walk collapses to these three writes.
            *density28 = 0.005;                            // 0x7ED7D8 / 0x7ED7DC
            *density20 = 0.0;                              // 0x5a39b6 / 0x5a39b9
            *density18 = 0.02 * half * 10.0;               // 0x7ED7E0, x 10.0
        }
        else if (*density28 == 0.0)                        // 0x5a39f5 - 0x5a3a03
        {
            // Taken by every non-shore cell on a fresh map: nothing writes
            // +0x28 before this stage and WorkCell's constructor zeroes it.
            // With the condition inverted the three densities stayed 0 and
            // ScatterLATBlobs produced nothing at all.
            *density28 = 0.005;
            *density18 = 0.02 * half;
            *density20 = 0.005;
        }
    }
}

// ---------------------------------------------------------------------------
// Step 2 - ScatterLATBlobs (sub_5A3AE0, 0x5A3AE0 - 0x5A4279).
//
//   1. clear work dword +0x3C on every cell;
//   2. draw three "blob count" uniforms n = F2I64(Random() * 4.889443517869674e-9
//      + 20.0) clipped to <= 40 (so 20..40 each); in draw order they are the
//      Rough, Sand and Green blob counts (var_4C / var_54 / var_3C);
//   3. for every cell that is a placeholder tile, flat (SlopeIndex == 0), with
//      no overlay, no object and work.byte(+0x45) == 0, take the FIRST branch
//      that fires:
//          u <  work.double(+0x18)                    -> RoughTile
//          u >= +0x18 and u <  +0x28                  -> SandTile
//          u >= +0x18 and u >= +0x28 and u <  +0x20   -> GreenTile
//          (all three thresholds cleared -> no blob at all)
//      and place a blob: PlaceBlob(cell, type, F2I64(clip(Gaussian() * span +
//      centre, [4, 80])), slot = x + (y << 9), false) with span/centre taken
//      from the matching n (20 / n, or the 38 / 42 fallback);
//   4. SetupLAT for every cell;
//   5. the tree-patch loop (see ScatterTrees);
//   6. the overlay tail: m = sub_42B1F0() / 200 = 2 * W * (H + 4) / 200,
//      n uniform in [0, m], then up to 5 * n iterations that pick a random work
//      slot, require the cell to have no overlay, write OverlayTypeIndex =
//      168 + (0..4) when the tile is in the ClearToSandLat family or
//      173 + (0..4) when it is a placeholder / green ground, then zero
//      cell[+0x11E] and call RecalcAttributes(cell, -1).
//
// RNG: three uniforms, then 1..3 per candidate cell, one Gaussian or more per
// blob, the patch loop's draws, and the overlay loop's two per iteration.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// IsClearToSandLatTile - sub_486790 (0x486790 - 0x4867B4).
//
//     return tile >= clearToSandLatIndex_ && tile < clearToSandLatIndex_ + 16;
//
// Note the vanilla does NOT guard against the family index being unset (-1),
// and neither does this: with an unset index the comparison would accept
// tiles 0..14, exactly as the engine's would.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsClearToSandLatTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;
    const int tile = cell->IsoTileTypeIndex;
    return tile >= clearToSandLatIndex_ && tile < clearToSandLatIndex_ + 16;
}

void RandomMapGenerator::ScatterLATBlobs()
{
    if (cellSlots_ == nullptr || workCells_ == nullptr)
        return;

    const int side = size_.workSide;
    const int slotCount = side * side;

    // One raw-uniform draw in [0, 1): the vanilla's
    // `Randomizer::Random * 2.328306437080797e-10`.
    const auto uniform = [&]() -> double
    {
        return static_cast<double>(static_cast<uint32_t>(rng_.Next()))
             * kUnitScale;
    };

    // ---- 1. clear the access mark (work dword +0x3C) on every cell ---------
    // 0x5a3af8 - 0x5a3b35.
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
            const int16_t y = static_cast<int16_t>(
                                  static_cast<uint32_t>(cell->MapCoords) >> 16);
            workCells_[x + side * y].data[15] = 0;
        }
    }

    // ---- 2. the three blob counts (0x5a3b47 - 0x5a3be6) --------------------
    // Each is F2I64(Random() * 21 * kUnitScale + 20.0), rejected while > 40,
    // i.e. uniform in 20..40. The draw order is Rough, Sand, Green - the three
    // stack slots the vanilla keeps them in are var_4C (0x5a3b82), var_54
    // (0x5a3bb2) and var_3C (0x5a3be2) and the rough branch loads var_4C
    // (0x5a3c8e), the sand branch var_54 (0x5a3d69) and the green branch var_3C
    // (0x5a3e45).
    const auto drawBlobCount = [&]() -> int
    {
        int n;
        do
        {
            n = F2I64(uniform() * 21.0 + 20.0);
        }
        while (n > 40);
        return n;
    };
    const int countRough = drawBlobCount();               // 0x5a3b62 var_4C
    const int countSand = drawBlobCount();                // 0x5a3b92 var_54
    const int countGreen = drawBlobCount();               // 0x5a3bc2 var_3C

    // The clipped Gaussian + PlaceBlob every branch ends with (0x5a3c92 -
    // 0x5a3d3a, 0x5a3d6d - 0x5a3e16, 0x5a3e49 - 0x5a3ef7): span 20 / centre n,
    // replaced by 38 / 42 when the window would not fit, clipped to [4, 80].
    const auto placeBlob = [&](MapCell* cell, int tileIndex, int centre, int slot)
    {
        double span = 20.0;
        double mean = static_cast<double>(centre);
        if (mean - 20.0 > 80.0 || mean + 20.0 < 4.0)
        {
            span = 38.0;
            mean = 42.0;
        }
        double size;
        do
        {
            size = rng_.Gaussian() * span + mean;
        }
        while (size < 4.0 || size > 80.0);
        PlaceBlob(cell, tileIndex, F2I64(size), slot, false);
    };

    // ---- 3. the LAT pass (0x5a3bea - 0x5a3f0a) ----------------------------
    // The comparison directions are the vanilla's. Each pair compares the raw
    // uniform draw against the density and then tests AH bit 0 (C0), which the
    // x87 sets only when the draw is BELOW the density:
    //     0x5a3c85 test ah,1 / 0x5a3c88 jz  -> uniform >= +0x18 -> nested levels
    //     0x5a3d60 test ah,1 / 0x5a3d63 jz  -> uniform >= +0x28 -> SandTile
    //     0x5a3e3c test ah,1 / 0x5a3e3f jz  -> uniform >= +0x20 -> no blob
    // The fall-through of the FIRST test runs into 0x5a3d34 `mov eax,
    // IsoTileTypeIndex_3` = roughTileIndex_, so the low-vegetation branch is the
    // ROUGH tile. The fall-through of the third runs into 0x5a3eea `mov edx,
    // IsoTileTypeIndex_1` = greenTileIndex_, so the third is the GREEN tile.
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
            const int16_t y = static_cast<int16_t>(
                                  static_cast<uint32_t>(cell->MapCoords) >> 16);
            WorkCell& w = workCells_[x + side * y];

            const double density18 =
                *reinterpret_cast<const double*>(w.data + 6);
            const double density20 =
                *reinterpret_cast<const double*>(w.data + 8);
            const double density28 =
                *reinterpret_cast<const double*>(w.data + 10);

            // 0x5a3c47: placeholder tile, flat, no overlay, no object and not
            // hill-protected.
            if (!IsPlaceholderTile(cell)
                || cell->SlopeIndex != 0
                || cell->OverlayTypeIndex != -1
                || (cell->AltFlags & AltCellFlags_ContainsBuilding) != 0
                || w.Byte(69) != 0)
                continue;

            const int slot = x + (y << 9);                // 0x5a3c67

            if (uniform() >= density18)                   // 0x5a3c88
            {
                if (uniform() >= density28)               // 0x5a3d63
                {
                    if (uniform() < density20)            // 0x5a3e3f
                        placeBlob(cell, greenTileIndex_, countGreen, slot);
                }
                else
                {
                    placeBlob(cell, sandTileIndex_, countSand, slot);
                }
            }
            else
            {
                placeBlob(cell, roughTileIndex_, countRough, slot);
            }
        }
    }

    // ---- 4. SetupLAT refresh (0x5a3f19 - 0x5a3f2e) ------------------------
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
            SetupLAT(cell);
    }

    // ---- 5. the green patch loop ------------------------------------------
    PlaceGreenPatches();                                  // 0x5a3f6c - 0x5a40bd

    // ---- 6. the overlay tail (0x5a40df - 0x5a4271) ------------------------
    // m = sub_42B1F0() / 200 = 2 * W * (H + 4) / 200, then at most n successful
    // plantings (n uniform in [0, m]) out of at most 5 * n attempts.
    const int overlayRange = (2 * size_.mapWidth * (size_.mapHeight + 4)) / 200;

    int overlayCount;
    do
    {
        overlayCount = F2I64(uniform() * static_cast<double>(overlayRange + 1));
    }
    while (overlayCount > overlayRange);                  // 0x5a4119

    int successes = 0;                                    // v37
    int attempts = 0;                                     // v35
    for (;;)
    {
        if (successes >= overlayCount)                    // 0x5a4133
            return;

        // A random work slot whose packed coords are not (0,0) - no attempt cap
        // here, unlike the patch loop above (0x5a4141 - 0x5a419c). The index
        // draw keeps the vanilla's `while (index > slotCount - 1)` rejection:
        // kUnitScale carries a (1 + 2^-32) factor, so a raw 0xFFFFFFFF really
        // can scale to exactly 1.0.
        int packed;
        do
        {
            int slot;
            do
            {
                slot = F2I64(uniform() * static_cast<double>(slotCount));
            }
            while (slot > slotCount - 1);
            packed = workCells_[slot].MapCoords();
        }
        while (packed == 0);

        const int16_t ovX = static_cast<int16_t>(packed & 0xFFFF);
        const int16_t ovY = static_cast<int16_t>(
                                   static_cast<uint32_t>(packed) >> 16);
        MapCell* cell = CellAt(ovX, ovY);

        // [移植侧] 出生点净场 6x6 内不放这种随机地面 overlay（168..177，
        // FA2 里表现为沙地上的碎石/土堆，即用户在 (109,111) 看到的"岩石"）。
        // 跳过本格继续重抽，不影响别处 overlay 数量目标。
        // 科技建筑地基格同样不铺：地基上不能有任何覆盖物。
        if (IsStartClearArea(ovX, ovY)
            || (cell->AltFlags & AltCellFlags_ContainsBuilding) != 0)
            continue;

        if (cell->OverlayTypeIndex == -1)                 // 0x5a41cd
        {
            int type = -1;
            if (IsClearToSandLatTile(cell))               // 0x5a41d5
            {
                int pick;
                do
                {
                    pick = F2I64(uniform() * 5.0);
                }
                while (pick > 4);
                type = pick + 168;
            }
            else if (IsPlaceholderTile(cell)
                     || IsGreenGroundTile(cell))          // 0x5a421f
            {
                int pick;
                do
                {
                    pick = F2I64(uniform() * 5.0);
                }
                while (pick > 4);
                type = pick + 173;
            }

            if (type >= 0)
                cell->OverlayTypeIndex = type;            // 0x5a424c

            // 0x5a424f zeroes the cell byte at +0x11E, which the port does not
            // model and never writes - a no-op here.
            RecalcAttributes(cell, -1);                   // 0x5a4259
            ++successes;                                  // 0x5a425e
        }

        ++attempts;
        if (attempts >= 5 * overlayCount)                 // 0x5a4271
            return;
    }
}

// ---------------------------------------------------------------------------
// Step 3 - ResetLATDensityFields (the inline loop at 0x59923E - 0x5992A9).
//
//     sub_578350();                           // iterator reset
//     while (cell = CellIteratorNext())
//     {
//         work = &workArray[80 * (x + workSide * y)];
//         work[+0x28] = dword_7ED7F0;         // the double at +0x28 = 0.005
//         work[+0x2C] = dword_7ED7F4;
//         work[+0x20] = dword_7ED7F8;         // the double at +0x20 = 0.001
//         work[+0x24] = dword_7ED7FC;
//     }
//
// The two doubles are 0x3F747AE147AE147B (0.005) and 0x3F50624DD2F1A9FC
// (0.001), read from the image (0x7ED7F0..0x7ED7FC). No RNG.
// ---------------------------------------------------------------------------
void RandomMapGenerator::ResetLATDensityFields()
{
    if (cellSlots_ == nullptr || workCells_ == nullptr)
        return;

    const int side = size_.workSide;

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);                  // 0x59923e sub_578350
    while (MapCell* cell = it.Next())                      // 0x599248
    {
        const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t y = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);
        WorkCell& w = workCells_[x + side * y];            // 0x599251 - 0x59927b

        // The vanilla stores four dwords (0x599283 - 0x599298):
        //   +0x28 / +0x2C = 0x7ED7F0 / 0x7ED7F4  ->  the double 0.005
        //   +0x20 / +0x24 = 0x7ED7F8 / 0x7ED7FC  ->  the double 0.001
        // Note it does NOT touch +0x18, so a shore cell keeps the value
        // PrepareLATDensities gave it.
        *reinterpret_cast<double*>(w.data + 10) = 0.005;
        *reinterpret_cast<double*>(w.data + 8) = 0.001;
    }
}

// ---------------------------------------------------------------------------
// Step 4 - ScatterRocks (sub_5A4280, 0x5A4280 - 0x5A45DD).
//
//   1. clear work dword +0x3C on every cell;
//   2. for every cell that is a placeholder tile, flat and unprotected
//      (work.byte(+0x45) == 0), draw once and on
//          Random() * 2^-32 < work.double(+0x28)      // 0.005
//      place a rock blob:
//          PlaceBlob(cell, roughTileIndex_ /* dword_ABC2B8 */,
//                    F2I64(clip(Gaussian() * 15.0 + 20.0, [4, 60])),
//                    x + (y << 9), false);
//   3. SetupLAT for every cell;
//   4. the same tree-patch loop as step 2.
//
// RNG: one uniform per candidate cell, one clipped Gaussian per rock, then the
// patch loop's draws.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// PlaceGreenPatches - the loop the vanilla INLINES TWICE: at the end of
// sub_5A3AE0 (0x5a3f6c - 0x5a40bd) and at the end of sub_5A4280 (0x5a4433 -
// 0x5a45cf), with identical code.
//
//     target = F2I64( (this[25] * 0.1 + 0.7) * (this[23] * 0.01) * this[191] )
//     while (target > 0)
//     {
//         if (patches >= 100) break;
//         for (attempts = 0; ; )                 // up to 200 attempts
//         {
//             do { slot = sub_598030(0, side*side - 1); }
//             while (work[slot].coords == (0,0));
//             ++attempts;
//             if (attempts > 200) { coords = (0,0); break; }
//             if (IsPlaceholderTile(GetCellAt(coords))) break;
//         }
//         size  = clip(Gaussian() * 0.1 + 0.2, [0.05, 0.4]);
//         count = F2I64(clip(Gaussian() * 10 + 25, [10, 35]));
//         target -= ScatterTrees(GetCellAt(coords), count, size);
//         ++patches;
//     }
//
// this[25] = config_.sizeSlider, this[23] = GlobalMapOptions::vegetation
// (60..100) and this[191] = RMGSettings::MaxTrees (600) - so the budget is
// 252..600 and the 100-patch cap always wins.
//
// Note the slot pick uses sub_598030, i.e. the port's RandomFloatRange (float
// scaling + upper rejection), NOT Randomizer::RandomRanged.
// ---------------------------------------------------------------------------
void RandomMapGenerator::PlaceGreenPatches()
{
    const int slotCount = size_.workSide * size_.workSide;

    int remaining = F2I64(
        (static_cast<double>(config_.sizeSlider) * 0.1 + 0.7)
        * (static_cast<double>(globalOptions_.vegetation) * 0.01)
        * static_cast<double>(settings_.MaxTrees));

    int patches = 0;
    while (remaining > 0)
    {
        if (patches >= 100)
            break;

        int16_t chosenX = 0;
        int16_t chosenY = 0;
        int attempts = 0;
        for (;;)
        {
            int packed;
            do
            {
                const int slot = rng_.RandomFloatRange(0, slotCount - 1);
                packed = workCells_[slot].MapCoords();
            }
            while (packed == 0);

            ++attempts;
            if (attempts > 200)
            {
                chosenX = 0;
                chosenY = 0;
                break;
            }

            chosenX = static_cast<int16_t>(packed & 0xFFFF);
            chosenY = static_cast<int16_t>(
                          static_cast<uint32_t>(packed) >> 16);
            if (IsPlaceholderTile(CellAt(chosenX, chosenY)))
                break;
        }

        double size;
        do
        {
            size = rng_.Gaussian() * 0.1 + 0.2;
        }
        while (size < 0.05 || size > 0.4);

        double count;
        do
        {
            count = rng_.Gaussian() * 10.0 + 25.0;
        }
        while (count < 10.0 || count > 35.0);

        remaining -= ScatterTrees(CellAt(chosenX, chosenY), F2I64(count), size);
        ++patches;
    }
}

void RandomMapGenerator::ScatterRocks()
{
    if (cellSlots_ == nullptr || workCells_ == nullptr)
        return;

    const int side = size_.workSide;

    const auto uniform = [&]() -> double
    {
        return static_cast<double>(static_cast<uint32_t>(rng_.Next()))
             * kUnitScale;
    };

    // ---- 1. clear the access mark (work dword +0x3C) on every cell ---------
    // 0x5a4298 - 0x5a42d5.
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
            const int16_t y = static_cast<int16_t>(
                                  static_cast<uint32_t>(cell->MapCoords) >> 16);
            workCells_[x + side * y].data[15] = 0;
        }
    }

    // ---- 2. the rock pass (0x5a42e7 - 0x5a43e6) ---------------------------
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
            const int16_t y = static_cast<int16_t>(
                                  static_cast<uint32_t>(cell->MapCoords) >> 16);
            WorkCell& w = workCells_[x + side * y];

            if (!IsPlaceholderTile(cell))                   // 0x5a4328
                continue;
            if (cell->SlopeIndex != 0 || w.Byte(69) != 0)   // 0x5a4341
                continue;
            // [移植侧] 岩石种子不选在出生点净场 6x6 内。
            if (IsStartClearArea(x, y))
                continue;
            // [移植侧] 岩石种子不选在科技建筑地基上（扩散时对地基格另有
            // 拦截，见 PlaceBlob；种子本身在扩散循环外，必须在这里挡掉，
            // 否则岩石地砖会直接盖到建筑下）。
            if ((cell->AltFlags & AltCellFlags_ContainsBuilding) != 0)
                continue;

            // 0x5a4382: the probability is work.double(+0x28), which the stage's
            // inline reset has just set to 0.005 on every cell.
            const double density28 =
                *reinterpret_cast<const double*>(w.data + 10);
            if (uniform() >= density28)
                continue;

            double size;
            do                                              // 0x5a439a - 0x5a43ba
            {
                size = rng_.Gaussian() * 15.0 + 20.0;
            }
            while (size < 4.0 || size > 60.0);

            // The rocks use RoughTile (dword_ABC2B8). The slot argument is the
            // vanilla's own expression, which carries the y << 16 term too
            // (sub_5A3AE0's equivalent is the clean x + (y << 9)); harmless,
            // since +0x3C is cleared per call and only compared for equality.
            PlaceBlob(cell, roughTileIndex_, F2I64(size),
                      cell->MapCoords + (static_cast<int>(y) << 9), false,
                      /*avoidStartClear=*/true);   // [移植侧] 岩石避开出生点
        }
    }

    // ---- 3. SetupLAT refresh (0x5a43f5 - 0x5a440a) ------------------------
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
            SetupLAT(cell);
    }

    // ---- 4. the same green patch loop -------------------------------------
    PlaceGreenPatches();                                    // 0x5a4433 - 0x5a45cf
}

// ---------------------------------------------------------------------------
// PlaceBlob - sub_5A4B60 (0x5A4B60 - 0x5A4FB0).
//
// Best-first (jittered-distance heap) growth of at most `count` cells from
// `cell`. Each accepted cell gets
//     cell->IsoTileTypeIndex = tileIndex;
//     work.dword(+0x3C)      = slotIndex;
// The seed itself is stamped with slotIndex first, which is what keeps the blob
// from re-entering itself; a neighbour is accepted when its work dword differs
// from slotIndex, it has no object, no overlay, no cell[+0x11C], and it is a
// placeholder tile - or, when `allowLatToLat` is set, a Pave / MiscPave tile.
// The heap key is sqrt(distance from the seed) + Random() * 5.0 * 2^-32.
//
// See 5A4B60_5A45E0.c for the full pseudocode (including the 20-byte heap
// control block and its sift-up/sift-down).
// ---------------------------------------------------------------------------
void RandomMapGenerator::PlaceBlob(MapCell* cell, int tileIndex, int count,
                                   int slotIndex, bool allowLatToLat,
                                   bool avoidStartClear)
{
    if (cell == nullptr || count <= 0)
        return;

    // The eight Neighbours offsets (0x89F688): N NE E SE S SW W NW.
    static const int16_t kDirX[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1,  0,  1,  1,  1,  0, -1 };

    // One 8-byte pool node: the packed coords and the float priority.
    // 0x5a4b72: 10 * count nodes; 0x5a4bcc: room for 10 * count + 1 pointers
    // (the heap is 1-based, so slot 0 stays unused).
    struct BlobNode
    {
        int   packed;
        float priority;
    };

    const int capacity = 10 * count;
    std::vector<BlobNode> pool(static_cast<size_t>(capacity));
    std::vector<int> heap;
    heap.push_back(-1);

    // sift-up insert (0x5a4c58 - 0x5a4cab). The vanilla refuses the node while
    // the heap already holds `capacity` entries.
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

    // Pop the root and sift down (sub_5AD870(1), 0x5a4cbc - 0x5a4cdd).
    const auto pop = [&]() -> int
    {
        if (heap.size() <= 1)
            return -1;
        const int root = heap[1];
        heap[1] = heap.back();
        heap.pop_back();
        const int last = static_cast<int>(heap.size()) - 1;
        int i = 1;
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

    const int16_t seedX = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int16_t seedY = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);

    int nextNode = 0;
    pool[nextNode].packed = cell->MapCoords;            // 0x5a4c23
    pool[nextNode].priority = 0.0f;                     // 0x5a4c25
    WorkAt(seedX, seedY).data[15] = slotIndex;          // 0x5a4c4f (+0x3C)
    push(nextNode);
    ++nextNode;

    int processed = 0;                                  // v45
    int current = pop();                                // 0x5a4cae - 0x5a4ce6
    while (current >= 0)
    {
        const int packed = pool[current].packed;
        const int16_t cx = static_cast<int16_t>(packed & 0xFFFF);
        const int16_t cy = static_cast<int16_t>(
                               static_cast<uint32_t>(packed) >> 16);

        // [移植侧] 岩石 blob 不盖出生点净场格，也不盖科技建筑地基格（扩散
        // 候选在下面的邻居循环里已排除地基格，但种子格不在候选循环内，要在
        // 这里一并挡住）。跳过盖章但继续扩散邻居。
        const bool stampBlocked
            = (avoidStartClear && IsStartClearArea(cx, cy))
           || ((CellAt(cx, cy)->AltFlags & AltCellFlags_ContainsBuilding) != 0);
        if (!stampBlocked)
            CellAt(cx, cy)->IsoTileTypeIndex = tileIndex;   // 0x5a4d13

        for (int k = 0; k < 8; ++k)                     // 0x5a4d44 - 0x5a4f42
        {
            const int16_t nx = static_cast<int16_t>(cx + kDirX[k]);
            const int16_t ny = static_cast<int16_t>(cy + kDirY[k]);
            if (!CellExists(nx, ny))
                continue;
            // [移植侧] 岩石 blob 不把净场格纳入候选，岩石不会漫进出生区。
            if (avoidStartClear && IsStartClearArea(nx, ny))
                continue;

            MapCell* nb = CellAt(nx, ny);
            if (!(IsPlaceholderTile(nb)
                  || (allowLatToLat && (IsMiscPaveTile(nb) || IsPaveTile(nb)))))
                continue;                               // 0x5a4dbf

            // The three acceptance clauses are the work dword +0x3C, the cell's
            // +0x11C and the cell's OverlayTypeIndex / FirstObject. The port
            // does not model cell +0x11C and never writes it, so a fresh cell
            // reads 0 and that clause always passes.
            if (WorkAt(nx, ny).data[15] == slotIndex)   // 0x5a4e1a
                continue;
            if (nb->OverlayTypeIndex != -1)
                continue;
            if ((nb->AltFlags & AltCellFlags_ContainsBuilding) != 0)
                continue;                               // FirstObject (+0xE4)

            const double dx = static_cast<double>(seedX - nx);
            const double dy = static_cast<double>(seedY - ny);
            const double distance = std::sqrt(dx * dx + dy * dy);   // 0x5a4e67
            const float key = static_cast<float>(
                distance
                + static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                      * 5.0 * kUnitScale);              // 0x5a4ea2

            pool[nextNode].packed = nb->MapCoords;      // 0x5a4e2f
            pool[nextNode].priority = key;
            WorkAt(nx, ny).data[15] = slotIndex;        // 0x5a4eba
            push(nextNode);                             // 0x5a4ec3 - 0x5a4f33
            ++nextNode;
        }

        ++processed;                                    // 0x5a4f4e
        if (processed >= count)                         // 0x5a4f63
            break;
        current = pop();                                // sub_5AC960
    }
}

// ---------------------------------------------------------------------------
// ScatterTrees - sub_5A45E0 (0x5A45E0 - 0x5A4ACD).
//
// Plants up to `count` TREE objects around `cell` and returns how many were
// planted (the caller subtracts that from its patch budget). Per popped cell:
//     if (!cell->FirstObject && cell->OverlayTypeIndex == -1
//         && cell->LandType != 3)
//         if (Random() * 2^-32 < probability)
//         {
//             do { n = F2I64(Random() * 5.820766092701993e-9 + 1.0); }
//             while (n > 25);                          // n uniform in 1..25
//             sprintf(name, "TREE%d%d", n / 10, n % 10);   // TREE01 .. TREE25
//             ... create the object (engine side) ...
//         }
// and pushes the 8 neighbours while
//     work.byte(+0x47) == 0 && work.byte(+0x45) == 0 && planted < 25 * count,
// marking each pushed cell work.byte(+0x47) = 1.
//
// The vanilla creates real ObjectClass instances (operator new(0xE0) +
// sub_71BB90 with the type from sub_71DD80). The port has no object system, so
// the route-B equivalent records one MapTerrainObject per tree (type name +
// cell), as the tiberium stage does for its TIBTRE decorations.
// ---------------------------------------------------------------------------
int RandomMapGenerator::ScatterTrees(MapCell* cell, int count,
                                     double probability)
{
    if (cell == nullptr || count <= 0)
        return 0;

    static const int16_t kDirX[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1,  0,  1,  1,  1,  0, -1 };

    // The vanilla builds the name with sprintf(name, "TREE%d%d", n / 10, n % 10)
    // for n in 1..25 (0x5a486f), i.e. exactly this set.
    static const char* const kTreeNames[25] =
    {
        "TREE01", "TREE02", "TREE03", "TREE04", "TREE05",
        "TREE06", "TREE07", "TREE08", "TREE09", "TREE10",
        "TREE11", "TREE12", "TREE13", "TREE14", "TREE15",
        "TREE16", "TREE17", "TREE18", "TREE19", "TREE20",
        "TREE21", "TREE22", "TREE23", "TREE24", "TREE25"
    };

    struct TreeNode
    {
        int   packed;
        float priority;
    };

    // 0x5a45f2: 25 * count nodes; 0x5a4657: room for 25 * count + 1 pointers.
    const int capacity = 25 * count;
    std::vector<TreeNode> pool(static_cast<size_t>(capacity));
    std::vector<int> heap;
    heap.push_back(-1);

    const auto push = [&](int node)                     // 0x5a46e6 - 0x5a473d
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

    const auto pop = [&]() -> int                       // 0x5a4740 - 0x5a477c
    {
        if (heap.size() <= 1)
            return -1;
        const int root = heap[1];
        heap[1] = heap.back();
        heap.pop_back();
        const int last = static_cast<int>(heap.size()) - 1;
        int i = 1;
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

    const int16_t seedX = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int16_t seedY = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);

    int nextNode = 1;                                   // v41 starts at 1
    pool[0].packed = cell->MapCoords;                   // 0x5a46b0
    pool[0].priority = 0.0f;                            // 0x5a46b2
    WorkAt(seedX, seedY).Byte(71) = 1;                  // 0x5a46dc
    push(0);

    int planted = 0;                                    // v42
    int current = pop();                                // 0x5a4740 - 0x5a4783
    while (current >= 0 && nextNode < capacity)         // 0x5a479f
    {
        const int packed = pool[current].packed;
        const int16_t cx = static_cast<int16_t>(packed & 0xFFFF);
        const int16_t cy = static_cast<int16_t>(
                               static_cast<uint32_t>(packed) >> 16);

        MapCell* c = CellAt(cx, cy);
        if (IsPlaceholderTile(c))                       // 0x5a47be
        {
            // 0x5a47ec: no object, no overlay, and the floor class is not the
            // third one (LandType != 3, cell +0xEC).
            const bool eligible
                = (c->AltFlags & AltCellFlags_ContainsBuilding) == 0
               && c->OverlayTypeIndex == -1
               && c->LandType != 3;

            if (eligible
                && static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                       * kUnitScale < probability)          // 0x5a4816
            {
                int type;
                do                                          // 0x5a4826 - 0x5a4848
                {
                    type = F2I64(
                        static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                        * 25.0 * kUnitScale + 1.0);
                }
                while (type > 25);

                // Route B: the vanilla creates an ObjectClass here
                // (operator new(0xE0) + sub_71BB90 with the type sub_71DD80
                // resolves). The port records one terrain object instead.
                terrainObjects_.push_back(
                    MapTerrainObject(CellStruct{ cx, cy }, kTreeNames[type - 1]));
                ++planted;                                  // 0x5a48af
            }
        }

        for (int k = 0; k < 8; ++k)                     // 0x5a48cc - 0x5a4a61
        {
            const int16_t nx = static_cast<int16_t>(cx + kDirX[k]);
            const int16_t ny = static_cast<int16_t>(cy + kDirY[k]);
            if (!CellExists(nx, ny))
                continue;

            WorkCell& w = WorkAt(nx, ny);
            // 0x5a496c: not already planted, not hill-protected, and the pool
            // still has room.
            if (w.Byte(71) != 0 || w.Byte(69) != 0)
                continue;
            if (nextNode >= capacity)
                continue;

            const double dx = static_cast<double>(seedX - nx);
            const double dy = static_cast<double>(seedY - ny);
            const double distance = std::sqrt(dx * dx + dy * dy);   // 0x5a49b1
            const float key = static_cast<float>(
                distance
                + static_cast<double>(static_cast<uint32_t>(rng_.Next()))
                      * 5.0 * kUnitScale);                  // 0x5a49eb

            // The vanilla stores the coords it computed, not the cell's own.
            pool[nextNode].packed = (static_cast<int>(nx) & 0xFFFF)
                                  | (static_cast<int>(ny) << 16);
            pool[nextNode].priority = key;
            w.Byte(71) = 1;                                 // 0x5a49ee
            push(nextNode);
            ++nextNode;                                     // 0x5a49dd
        }

        current = pop();
        if (planted >= count)                           // 0x5a4aaa
            break;
    }
    return planted;                                     // 0x5a4aca
}

// ---------------------------------------------------------------------------
// PruneTerrainTrees - 移植侧：最终瓦片定型后剔除放错位置的树。
//
// 为什么放在第 4 次 RecalculateCellAttributes 之后：种树（ScatterTrees）在
// CreateLATs 阶段，那时悬崖 / 水体多格大片 footprint 内的覆盖格还是
// 0xFFFF（"被多格片占位"），与真正的空地 tile 0 无法区分，树会被种进这些
// 格；等多格片落定，它们就是水面或悬崖墙。只有在所有瓦片定型后按最终格
// 属性判定才能可靠剔除。
//
// 剔除规则（只作用于 TREE01..25，TIBTRE 矿树不动）：
//   1. 格不存在 / tile == 0xFFFF（多格片覆盖，非可站立地面）；
//   2. 水面格（IsWaterTile）；
//   3. 真悬崖墙格：CliffSet（shoreTileIndex_ 起 40 宽）或 WaterCliffs
//      （waterCliffsIndex_ 起 28 宽）。坡一律保留——RampBase / CliffRamps /
//      RampSmooth 都是可行走坡面，不在剔除区间内。
//   4. 出生点周围少放：切比雪夫距离 <= kTreeClearRadius（2）的全删，给开局
//      建筑留地；kTreeClearRadius < d <= kTreeSparseRadius（5）的按棋盘格
//      （x+y 为偶）删掉一半做稀疏；更远不动。
// ---------------------------------------------------------------------------
void RandomMapGenerator::PruneTerrainTrees()
{
    static const int kTreeClearRadius  = 2;   // 出生点四周此距离内不放树
    static const int kTreeSparseRadius = 5;   // 到此距离内做棋盘格稀疏

    const auto isTreeObject = [](const char* name) -> bool
    {
        // 仅 TREE01..TREE25；TIBTRE01..03 前缀是 "TIBT"，不会命中。
        return name != nullptr
            && std::strncmp(name, "TREE", 4) == 0
            && name[4] >= '0' && name[4] <= '9';
    };

    const auto onBlockingTerrain = [&](MapCell* c) -> bool
    {
        if (c == nullptr)
            return true;                       // 格不存在
        const int tile = c->IsoTileTypeIndex;
        if (tile == 0xFFFF)
            return true;                       // 多格片覆盖格
        if (IsWaterTile(c))
            return true;                       // 水面
        if (shoreTileIndex_ != -1
            && tile >= shoreTileIndex_ && tile < shoreTileIndex_ + 40)
            return true;                       // CliffSet 悬崖墙（40 宽）
        if (waterCliffsIndex_ != -1
            && tile >= waterCliffsIndex_ && tile < waterCliffsIndex_ + 28)
            return true;                       // WaterCliffs 水崖（28 宽）
        return false;                          // 含坡：RampBase/CliffRamps/...
    };

    const auto nearStartTooClose = [&](int x, int y) -> int
    {
        // 返回 0=保留，1=清空，2=棋盘稀疏（由调用方看 x+y 奇偶）。
        int best = kTreeSparseRadius + 1;
        for (const StartingPointRecord& sp : startingPoints_)
        {
            const int d = (std::max)(std::abs(x - sp.coords.X),
                                     std::abs(y - sp.coords.Y));
            best = (std::min)(best, d);
        }
        return best;
    };

    size_t removedTerrain = 0;
    size_t removedStart   = 0;
    size_t removedBuilding = 0;   // [移植侧] 压在科技建筑地基上的地形对象

    DiagLog("TREE-PRUNE-BASE water=%d shore=%d waterCliffs=%d",
            waterTileIndex_, shoreTileIndex_, waterCliffsIndex_);

    terrainObjects_.erase(
        std::remove_if(terrainObjects_.begin(), terrainObjects_.end(),
            [&](MapTerrainObject& obj) -> bool
            {
                const int x = obj.coords.X;
                const int y = obj.coords.Y;
                MapCell* c = CellExists(x, y) ? CellAt(x, y) : nullptr;

                // [移植侧] 最高优先级：任何地形对象（树、TIBTRE 矿柱，不论
                // 种类）都不允许压在科技建筑地基上。各生成阶段已有避让，这里
                // 是写盘前的最后一道兜底。
                if (c != nullptr
                    && (c->AltFlags & AltCellFlags_ContainsBuilding) != 0)
                {
                    ++removedBuilding;
                    DiagLog("TREE-PRUNE-BUILDING (%d,%d) %s tile=%d",
                            x, y, obj.typeName ? obj.typeName : "(null)",
                            c->IsoTileTypeIndex);
                    return true;
                }

                if (!isTreeObject(obj.typeName))
                    return false;               // 非树（矿树等）一律保留

                const int tileNow = c ? c->IsoTileTypeIndex : -1;

                if (onBlockingTerrain(c))
                {
                    ++removedTerrain;
                    DiagLog("TREE-PRUNE-DEL (%d,%d) %s tile=%d",
                            x, y, obj.typeName, tileNow);
                    return true;
                }

                // [TEMP DIAG] 视觉水/崖硬编码区间 vs 运行时基址判据对不上时打印。
                if ((tileNow >= 314 && tileNow <= 327)
                    || (tileNow >= 49 && tileNow <= 88))
                {
                    DiagLog("TREE-PRUNE-MISMATCH (%d,%d) %s tile=%d water=%d..%d "
                            "shore=%d..%d wc=%d..%d",
                            x, y, obj.typeName, tileNow,
                            waterTileIndex_, waterTileIndex_ + 13,
                            shoreTileIndex_, shoreTileIndex_ + 39,
                            waterCliffsIndex_, waterCliffsIndex_ + 27);
                }

                const int d = nearStartTooClose(x, y);
                if (d <= kTreeClearRadius)
                {
                    ++removedStart;
                    return true;                // 出生点紧邻：清空
                }
                if (d <= kTreeSparseRadius && (((x + y) & 1) == 0))
                {
                    ++removedStart;
                    return true;                // 近圈：棋盘格稀疏一半
                }
                return false;
            }),
        terrainObjects_.end());

    DiagLog("TREE-PRUNE terrain=%zu start=%zu building=%zu kept=%zu",
            removedTerrain, removedStart, removedBuilding,
            terrainObjects_.size());

    // [移植侧] 最后兜底：科技建筑地基格上不允许残留任何 overlay（矿石、
    // 碎石 LAT 等）。各生成阶段已直接避让，这一遍只清除漏网之鱼，并记一条
    // 日志便于发现"避让失效"的路径。
    if (cellSlots_ != nullptr)
    {
        int clearedOverlay = 0;
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth, slotStride_);
        while (MapCell* cell = it.Next())
        {
            if ((cell->AltFlags & AltCellFlags_ContainsBuilding) != 0
                && cell->OverlayTypeIndex != -1)
            {
                DiagLog("TREE-PRUNE-BUILDING-OVERLAY (%d,%d) overlay=%d",
                        static_cast<int>(cell->MapCoords & 0xFFFF),
                        static_cast<int>(
                            static_cast<uint32_t>(cell->MapCoords) >> 16),
                        cell->OverlayTypeIndex);
                cell->OverlayTypeIndex = -1;
                cell->OverlayData = 0;
                ++clearedOverlay;
            }
        }
        DiagLog("TREE-PRUNE building overlays swept=%d", clearedOverlay);
    }
}

// ---------------------------------------------------------------------------
// IsStartClearArea - 出生点净场 6x6 判定（移植侧）。
//
// 出生点 2x2（MCV 占地）居中、四周外扩 kStartClearOuter 格 => 边长
// 2 + 2*kStartClearOuter = 6。CreateStartingPoints 在 AddTiberium 与
// CreateLATs 之前运行，故矿石、矿柱、树、岩石生成时直接调用它跳过这些格，
// 不改写已经铺好的 LAT / 地形。
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsStartClearArea(int x, int y) const
{
    static const int kStartClearOuter = 2;   // 6x6
    for (const StartingPointRecord& sp : startingPoints_)
    {
        if (x >= sp.coords.X - kStartClearOuter
            && x <= sp.coords.X + kStartClearOuter + 1
            && y >= sp.coords.Y - kStartClearOuter
            && y <= sp.coords.Y + kStartClearOuter + 1)
        {
            return true;
        }
    }
    return false;
}

// 矿石/矿柱净场：出生点 2x2 外扩 8 格 => 18x18（默认口径）。
// 群岛例外：外扩 3 格 => 8x8，这是群岛特有的规则。
bool RandomMapGenerator::IsOreClearArea(int x, int y) const
{
    static const int kOreClearOuter = 8;              // 默认 18x18
    static const int kOreClearOuterIsles = 3;         // 群岛 8x8
    const int outer = (config_.landType == RmLandType::Archipelago)
                          ? kOreClearOuterIsles
                          : kOreClearOuter;
    for (const StartingPointRecord& sp : startingPoints_)
    {
        if (x >= sp.coords.X - outer
            && x <= sp.coords.X + outer + 1
            && y >= sp.coords.Y - outer
            && y <= sp.coords.Y + outer + 1)
        {
            return true;
        }
    }
    return false;
}
