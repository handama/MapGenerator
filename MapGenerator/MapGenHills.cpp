// ============================================================================
// MapGenHills.cpp - the "RMG: Creating hills" stage
// (sub_598960 @ 0x599171 - 0x599214) and its driver sub_5A35F0.
//
// The stage slice is a single unconditional call (no land-type gate):
//
//     0x599171  push "RMG: Creating hills\n", nullsub_1     debug
//     0x599184  sub_5A35F0(this)                  // the whole stage body
//     0x5991A1  ... UI / session tail (NOT ported): sub_69AE90, sub_643C50,
//               psub_48D1D0, sub_641140, SendMessageA(WM_PAINT), sub_5E7EB0 ...
//     0x599215  "RMG: Creating LATs, rocks etc"             next stage
//
// Full annotated walkthrough: 599171_CreatingHills.c, 5A35F0.c, 5A33F0.c,
// 5A2F50.c and 6B2A70_6B3850_6B4100_6B4240_6B3E60.c in the decompile folder.
//
// ----------------------------------------------------------------------------
// THE STAGE BODY IS A SIX-STEP SEQUENCE (sub_5A35F0)
// ----------------------------------------------------------------------------
//   sub_5A35F0's own instruction order is exactly this, and CreateHills below
//   mirrors it one call per step:
//
//     0x5a35f9  1. sub_5A33F0()   shore / cliff protection pre-mark
//     0x5a3600  2. sub_5A2F50(this)   height field + amplitude field
//     0x5a3605  3. sub_6B2A70()   build the elevation grid for the map
//     0x5a361a  4. the per-cell level-apply loop (inline; walks all workSide^2
//                 slots and drives sub_6B4100 / sub_6B4240 / sub_6B3E60)
//     0x5a36c1  5. sub_6B3850()   final slope pass (Level / SlopeIndex / tile)
//     0x5a36cb  6. the 2x2 slope merge (inline; slope 5 -> 5/6/7/8 and
//                 slope 11 -> 11/12/9/10 blocks become 0xFFFF + level bump)
//
//   All of the stage's RNG lives in step 2 (two clipped Gaussian rejection
//   loops per processed work cell); nothing else draws.
//
//   Because the stage itself only hands cells to those helpers, the port keeps
//   the same shape: CreateHills is the (already complete) skeleton and each of
//   the six steps is a separate translation unit-level function whose body is
//   still to be written. Every one of them is declared in MapGen.h and called
//   here; none of them has content yet.
//
// ----------------------------------------------------------------------------
// SUBORDINATE STATUS
// ----------------------------------------------------------------------------
//   implemented : every step -
//                 MarkHillProtection      sub_5A33F0
//                 BuildHillHeightField    sub_5A2F50
//                 BuildElevationGrid      sub_6B2A70
//                 ApplyElevationLevels    the inline 0x5A361A loop, with
//                                         sub_6B4100 / sub_6B4240 / sub_6B3E60
//                                         / sub_6B3A80
//                 FinalizeElevationSlopes sub_6B3850
//                 MergeSlopeBlocks        the inline 0x5A36CB merge
//   stub        : none
//
//   Dependencies, all resolved (each documented in the decompile files):
//     sub_6B4100 (current level) / sub_6B4240 (raise-lower mask) /
//     sub_6B3E60 (apply one step, with an undo journal) - used by
//     ApplyElevationLevels;
//     sub_6B2520, sub_6B3A80, sub_58C2A0 (work-cell accessor = WorkAt), sub_5AC230
//     - used inside BuildElevationGrid / the apply path;
//     the 19-entry slope-pattern table at 0x83FF18, the elevation grid
//     dword_B0B6EC with Left / Top / dword_8759A4, and the two tile globals
//     dword_ABC1D8 (RampBase) and IsoTileTypeIndex_0 (ClearTile) - the port
//     already carries the last two as rampBaseIndex_ / clearTileIndex_;
//     sub_4865B0 (shore family) and sub_4863D0 (cliff family) - already modelled.
// ============================================================================

#include "pch.h"
#include "MapGen.h"

// ---------------------------------------------------------------------------
// The stage body - sub_5A35F0 (0x5A35F0 - 0x5A38B0).
// ---------------------------------------------------------------------------
void RandomMapGenerator::CreateHills()
{
    MarkTechBuildingProtection(); // [port-only] keep tech buildings on flat ground
    MarkHillProtection();        // 0x5a35f9  sub_5A33F0
    BuildHillHeightField();      // 0x5a3600  sub_5A2F50
    BuildElevationGrid();        // 0x5a3605  sub_6B2A70
    ApplyElevationLevels();      // 0x5a361a  the inline level-apply loop
    FinalizeElevationSlopes();   // 0x5a36c1  sub_6B3850
    MergeSlopeBlocks();          // 0x5a36cb  the inline 2x2 slope merge
    RemoveCliffsOverRamps();     // port-only: keep ramp seams free of walls
}

// ---------------------------------------------------------------------------
// MarkTechBuildingProtection [port-only]
//
// Tech buildings (AddTechBuildings) run BEFORE this stage. Each foundation cell
// is flagged AltCellFlags_ContainsBuilding, which makes CanHostSlope fail for
// that cell alone: its own level and tile stay put, but every neighbour is still
// free game. The height field can then raise a plateau around the building and
// the final slope pass carves its ramp band on the ring of cells touching the
// foundation - the building ends up surrounded on all sides by slope tiles and
// looks planted in the side of a hill (measured on rmg_20261003_121340: the
// 3x3 CAAIRP foundation had 510..520 ramp tiles on every edge).
//
// Fix: paint the hill protection mark (work byte +69, the same one
// MarkHillProtection uses for shore / cliff neighbours and the starting-point
// flood uses) on every foundation cell and on every cell of an apron around
// it. The height-field pass then forces those cells to ground level with a
// near-zero amplitude, BuildElevationGrid blocks the elevation-grid corners
// they touch, and FinalizeElevationSlopes never stamps a slope tile on them -
// the whole hill (raise AND band) is pushed outside the apron.
//
// The apron is two cells wide: that is the same clearance the starting-point
// area uses (a 2x2 MCV foundation plus a 2-cell outer ring = the 6x6 start
// clear area), and it keeps one full flat row between the building art and the
// first ramp tile.
// ---------------------------------------------------------------------------
void RandomMapGenerator::MarkTechBuildingProtection()
{
    if (cellSlots_ == nullptr || workCells_ == nullptr)
        return;

    const int side = size_.workSide;
    const int kApron = 2;                          // flat cells kept around it
    int foundations = 0;
    int marked = 0;

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())
    {
        if ((cell->AltFlags & AltCellFlags_ContainsBuilding) == 0)
            continue;
        ++foundations;

        const int16_t bx = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t by = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);

        for (int dy = -kApron; dy <= kApron; ++dy)
        {
            for (int dx = -kApron; dx <= kApron; ++dx)
            {
                const int16_t x = static_cast<int16_t>(bx + dx);
                const int16_t y = static_cast<int16_t>(by + dy);
                if (!CellExists(x, y))
                    continue;
                WorkCell& w = workCells_[x + side * y];
                if (w.Byte(69) == 0)
                {
                    w.Byte(69) = 1;
                    ++marked;
                }
            }
        }
    }

    DiagLog("HILL-PROT tech foundations=%d newly marked apron cells=%d",
            foundations, marked);
}

// ---------------------------------------------------------------------------
// Step 1 - MarkHillProtection (sub_5A33F0, 0x5A33F0 - 0x5A35EB).
//
// For every shore tile (sub_4865B0) the first placeholder-tile neighbour in
// Neighbours order gets its height field seeded with the double 0.5
// (work +8 / +12 = 0 / 0x3FE00000) and its protection mark (work byte +69) set.
// For every cliff-family tile (sub_4863D0) the same neighbour only gets the
// protection mark.
//
// Already-modelled dependencies: the shore / cliff family tests and
// sub_486380 (placeholder tile).
// ---------------------------------------------------------------------------
void RandomMapGenerator::MarkHillProtection()
{
    if (cellSlots_ == nullptr || workCells_ == nullptr)
        return;

    const int side = size_.workSide;
    static const int16_t kDirX[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1,  0,  1,  1,  1,  0, -1 };

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())
    {
        const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t y = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);

        // ---- shore tile (0x5a3417 sub_4865B0) -----------------------------
        if (IsShoreTile(cell))
        {
            for (int d = 0; d < 8; ++d)                   // 0x5a3431 Neighbours order
            {
                const CellStruct nb{
                    static_cast<int16_t>(x + kDirX[d]),
                    static_cast<int16_t>(y + kDirY[d]) };
                if (!CellExists(nb.X, nb.Y))              // 0x5a3483 diamond test
                    continue;
                if (!IsPlaceholderTile(CellAt(nb.X, nb.Y)))   // 0x5a3496 sub_486380
                    continue;

                WorkCell& w = workCells_[nb.X + side * nb.Y];
                // 0x5a34ce / 0x5a34dc: the two dwords 0 and 0x3FE00000, i.e. the
                // height field (work +8) seeded with the double 0.5.
                *reinterpret_cast<double*>(w.data + 2) = 0.5;
                w.Byte(69) = 1;                           // 0x5a3503
                break;                                    // 0x5a3508
            }
            continue;
        }

        // ---- cliff-family tile (0x5a350f sub_4863D0) ----------------------
        if (IsCliffFamilyTile(cell))
        {
            for (int d = 0; d < 8; ++d)
            {
                const CellStruct nb{
                    static_cast<int16_t>(x + kDirX[d]),
                    static_cast<int16_t>(y + kDirY[d]) };
                if (!CellExists(nb.X, nb.Y))
                    continue;
                if (!IsPlaceholderTile(CellAt(nb.X, nb.Y)))
                    continue;

                workCells_[nb.X + side * nb.Y].Byte(69) = 1;  // 0x5a35ca
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Step 2 - BuildHillHeightField (sub_5A2F50, 0x5A2F50 - 0x5A33EF).
//
// Gated on this[17] * 0.0025 >= 0.025 (i.e. the hill-strength parameter >= 10).
// Blends a height field (work +8, double, a DELTA) and an amplitude field
// (work +16, double) from the west / north neighbours, clips the amplitude
// delta, forces the protected cells' height to >= 0 and their amplitude to
// 0.0025, then applies two clipped Gaussian perturbation draws per cell, and
// finally rounds the height field to (double)F2I64(x +- 0.5).
//
// This is the only RNG consumer of the stage; the Gaussian is sub_5980C0 =
// the port's rng_.Gaussian().
// ---------------------------------------------------------------------------
void RandomMapGenerator::BuildHillHeightField()
{
    if (workCells_ == nullptr)
        return;

    // this[17], the hill strength rolled by sub_597260 (see GlobalMapOptions).
    const double strength = static_cast<double>(globalOptions_.ruggedness);
    const double ampLo = strength * 0.0025;             // 0x5a2f88
    const double jitter = strength * 0.0001 + 0.1;      // 0x5a2f76
    const double ampHi = strength * 0.005;              // 0x5a2f92
    if (ampLo < 0.025)                                  // 0x5a2fa8
        return;                    // never taken: sub_597260 rolls this[17] in [20, 100]

    const int side = size_.workSide;
    const int count = side * side;

    // The two doubles a work cell carries: the HEIGHT field at +8 and the
    // AMPLITUDE field at +16. WorkCell stores 20 ints, so they are reached
    // through the raw bytes (the vanilla does the same qword fp loads/stores).
    const auto height = [&](int index) -> double& {
        return *reinterpret_cast<double*>(workCells_[index].data + 2);
    };
    const auto amplitude = [&](int index) -> double& {
        return *reinterpret_cast<double*>(workCells_[index].data + 4);
    };

    // ---- pass A (0x5a2fc7 - 0x5a3383) -------------------------------------
    for (int i = 0; i < count; ++i)
    {
        const int packed = workCells_[i].MapCoords();   // 0x5a2fc9
        if (packed == 0)                                // 0x5a2fd3 / 0x5a2fdb
            continue;

        const int16_t x = static_cast<int16_t>(packed & 0xFFFF);
        const int16_t y = static_cast<int16_t>(
                              static_cast<uint32_t>(packed) >> 16);
        const int16_t xm = static_cast<int16_t>(x - 1);
        const int16_t ym = static_cast<int16_t>(y - 1);

        amplitude(i) = 0.0;                             // 0x5a2fe7 / 0x5a2ff0

        // The reference height: the north-west cell (x-1, y-1).
        double reference = 0.0;                         // var_40
        if (CellExists(xm, ym))                         // 0x5a3034
            reference = height(xm + side * ym);         // 0x5a3053

        // The west cell (x-1, y): it sets the amplitude (or ampLo when it lies
        // off the diamond) and seeds the delta (0x5a305b - 0x5a30d2).
        double delta = 0.0;                             // v30 / var_58
        if (CellExists(xm, y))
        {
            height(i) += height(xm + side * y);         // 0x5a309c
            amplitude(i) = amplitude(xm + side * y);    // 0x5a30a2 / 0x5a30ab
            delta = height(xm + side * y) - reference;  // 0x5a30ae
        }
        else
        {
            amplitude(i) = ampLo;                       // 0x5a30cc / 0x5a30d2
        }

        // The north cell (x, y-1) adds in, or ampLo when off the diamond
        // (0x5a30df - 0x5a3151).
        if (CellExists(x, ym))
        {
            height(i) += height(x + side * ym);         // 0x5a311e
            amplitude(i) += amplitude(x + side * ym);   // 0x5a3127
            delta += height(x + side * ym) - reference; // 0x5a3130 / 0x5a3134
        }
        else
        {
            amplitude(i) += ampLo;                      // 0x5a3145
        }

        // Average with what the cell already carried: zero for an ordinary
        // cell, the 0.5 seed sub_5A33F0 left on a protected one
        // (0x5a3154 - 0x5a3169).
        height(i) *= 0.5;
        amplitude(i) *= 0.5;

        // The delta is only ever used as a sign (0x5a316c - 0x5a31a6).
        if (delta > 0.0)
            delta = jitter;
        else if (delta < 0.0)
            delta = -jitter;

        // A protected cell keeps a non-negative height and a 0.0025 amplitude
        // (0x5a31aa - 0x5a31d9).
        if (workCells_[i].Byte(69) != 0)
        {
            if (height(i) < 0.0)
                height(i) = 0.0;                        // 0x5a31bc / 0x5a31c6
            amplitude(i) = 0.0025;                      // 0x5a31c9 / 0x5a31d0
        }

        // ---- draw 1, the amplitude (0x5a31e0 - 0x5a3281) ------------------
        {
            const double lo = -amplitude(i);            // v32
            const double hi = ampHi - amplitude(i);     // v24
            double span = 0.025;                        // v36
            double centre = 0.0;                        // v38
            if (-0.025 > hi || lo > 0.025)              // 0x5a3237
            {
                span = (hi - lo) * 0.5;                 // 0x5a3247
                centre = span - amplitude(i);           // 0x5a324f
            }
            double d;
            do                                          // 0x5a3272 / 0x5a3281
                d = rng_.Gaussian() * span + centre;
            while (d < lo || d > hi);
            amplitude(i) += d;                          // 0x5a328a
        }

        // ---- draw 2, the height (0x5a329a - 0x5a335d) ---------------------
        {
            const double lo = -2.0 - height(i);         // v25
            const double hi = 2.0 - height(i);          // v33
            double centre = (delta < lo) ? lo
                          : ((delta > hi) ? hi : delta); // v16
            double span = amplitude(i);                 // v31: the updated amplitude
            if (centre - amplitude(i) > hi || amplitude(i) + centre < lo)
            {                                           // 0x5a330f
                span = (hi - lo) * 0.5;                 // 0x5a3323
                centre = span + lo;                     // 0x5a332b
            }
            double d;
            do                                          // 0x5a334e / 0x5a335d
                d = rng_.Gaussian() * span + centre;
            while (d < lo || d > hi);
            height(i) += d;                             // 0x5a336c
        }
    }

    // ---- pass B, round the height field (0x5a338d - 0x5a33e6) -------------
    // Every work slot is rounded, not just the ones pass A touched: the driver
    // reads the field back as the integer height step.
    for (int i = 0; i < count; ++i)
    {
        const double h = height(i);
        if (h > 0.0)
            height(i) = static_cast<double>(F2I64(h + 0.5));   // 0x5a33dc
        else if (h < 0.0)
            height(i) = static_cast<double>(F2I64(h - 0.5));   // 0x5a33b8
        // h == 0 is left as it is
    }
}

// ---------------------------------------------------------------------------
// Step 3 - BuildElevationGrid (sub_6B2A70, 0x6B2A70 - 0x6B2F28).
//
// Allocates the scratch elevation grid over MouseClass's MapCoordBounds -
// (Right + 1) * (Bottom + 1) entries of 8 bytes, with Left / Top /
// dword_8759A4 (the row stride = Right + 1) as its origin and stride - and
// seeds each entry from the map cell's own elevation
//     15 * cell->Level + off_83FF18[cell->SlopeIndex][0]
// plus the "blocked" byte for the grid points whose walk left the diamond or
// whose neighbouring cells fail the usable / overlay / object / +69 / tile
// tests.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The slope-pattern table at 0x83FF18: 19 POINTERS, each to four corner
// offsets, in the order the four grid points are read - e(x, y), e(x + 1, y),
// e(x + 1, y + 1), e(x, y + 1).
//
// Index 0 points at 0xB0B6DC, a 16-byte global that is zero in the image and
// whose only xref is the table itself (nothing ever writes it), so its pattern
// is four zeros - NOT a -1 sentinel. It matters twice over:
//   * sub_6B2A70 seeds a grid point with
//         15 * cell->Level + off_83FF18[cell->SlopeIndex][0]   // 0x6B2C95
//     and SlopeIndex 0 is what an ordinary level cell carries, so a flat cell
//     seeds 15 * Level and not 15 * Level - 1;
//   * sub_6B3850's lookup starts at SlopeIndex 0 (0x6B39F7) and breaks on the
//     match, so a flat cell really does get SlopeIndex 0 and the clear tile
//     (0x6B3A33) instead of being left with its previous tile.
// Indices 1..18 are the sloped patterns.
// ---------------------------------------------------------------------------
static const int kSlopeCorners[19][4] =
{
    {  0,  0,  0,  0 },     //  0
    {  0, 15, 15,  0 },     //  1
    {  0,  0, 15, 15 },     //  2
    { 15,  0,  0, 15 },     //  3
    { 15, 15,  0,  0 },     //  4
    {  0,  0, 15,  0 },     //  5
    {  0,  0,  0, 15 },     //  6
    { 15,  0,  0,  0 },     //  7
    {  0, 15,  0,  0 },     //  8
    {  0, 15, 15, 15 },     //  9
    { 15,  0, 15, 15 },     // 10
    { 15, 15,  0, 15 },     // 11
    { 15, 15, 15,  0 },     // 12
    {  0, 15, 30, 15 },     // 13
    { 15,  0, 15, 30 },     // 14
    { 30, 15,  0, 15 },     // 15
    { 15, 30, 15,  0 },     // 16
    {  0, 15,  0, 15 },     // 17
    { 15,  0, 15,  0 },     // 18
};

// ---------------------------------------------------------------------------
// IsMorphableTile - the vanilla's morphable clause on an UNSIGNED index:
//     tile < IsometricTileTypeClass::Array.Count || tile == 0xFFFF
//         || Array.Items[tile]->Morphable
// so an unset index (0xFFFFFFFF) or anything past the end answers true.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsMorphableTile(int tile) const
{
    const unsigned int index = static_cast<unsigned int>(tile);
    if (index >= tileMorphable_.size())
        return true;
    return tileMorphable_[index] != 0;
}

// ---------------------------------------------------------------------------
// CanHostSlope - sub_6B2520 (0x6B2520 - 0x6B2563).
// ---------------------------------------------------------------------------
bool RandomMapGenerator::CanHostSlope(int16_t x, int16_t y)
{
    if (!CellExists(x, y))
        return false;

    const MapCell* cell = CellAt(x, y);
    if (cell->OverlayTypeIndex != -1)          // sub_6B2520's this[17] (+0x44)
        return false;
    if ((cell->AltFlags & AltCellFlags_ContainsBuilding) != 0)
        return false;                          // this[57] (+0xE4) FirstObject
    if (WorkAt(x, y).Byte(69) != 0)
        return false;

    return IsMorphableTile(cell->IsoTileTypeIndex);
}

// Step 3 - BuildElevationGrid (sub_6B2A70, 0x6B2A70 - 0x6B2F28).
//
// Allocates the scratch elevation grid over MouseClass's MapCoordBounds -
// (Right + 1) * (Bottom + 1) entries of 8 bytes, with Left / Top /
// dword_8759A4 (the row stride = Right + 1) as its origin and stride - and
// seeds each entry from the map cell's own elevation
//     15 * cell->Level + off_83FF18[cell->SlopeIndex][0]
// plus the "blocked" byte for the grid points whose walk left the diamond or
// whose neighbouring cells fail the usable / overlay / object / +69 / tile
// tests.
//
// The rectangle: the vanilla's MapCoordBounds is the map's cell rectangle. The
// port's own iterator covers x in [0, W' + H' - 1] and y in [0, W' + H'] (see
// MapGen.cpp's cellSlots_ setup), which is exactly the span the grid has to
// cover for every diamond cell to be addressable - so Left = Top = 0,
// stride = W' + H', rows = W' + H' + 1.
// ---------------------------------------------------------------------------
void RandomMapGenerator::BuildElevationGrid()
{
    if (cellSlots_ == nullptr)
        return;

    // 0x6b2a9e / 0x6b2b3b: the grid is (Right + 1) x (Bottom + 1) entries with
    // the row stride Right + 1. The diamond's cells span x, y in [1, W'+H'-1]
    // and every cell reads the four grid points (x, y) .. (x + 1, y + 1), so the
    // grid has to carry gx, gy in [0, W'+H'] - i.e. W'+H'+1 = workSide of both.
    // (With the stride one short, the rightmost column's e(x+1, ..) probes wrap
    // into the next row instead of reading their own corner.)
    const int side = size_.workSide;                     // Right + 1 = W' + H' + 1
    const int rows = size_.workSide;                     // Bottom + 1

    elevLeft_ = 0;
    elevTop_ = 0;
    elevStride_ = side;

    // 0x6b2a7e: a previous grid is freed first.
    elevGrid_.assign(static_cast<size_t>(side) * rows, ElevationEntry());
    for (size_t i = 0; i < elevGrid_.size(); ++i)
    {
        elevGrid_[i].corner = 0;
        elevGrid_[i].blocked = 0;
        elevGrid_[i].touched = 0;
    }

    for (int gy = 0; gy < rows; ++gy)
    {
        for (int gx = 0; gx < side; ++gx)
        {
            ElevationEntry& entry = elevGrid_[gx + elevStride_ * gy];

            const int16_t bx = static_cast<int16_t>(elevLeft_ + gx);
            const int16_t by = static_cast<int16_t>(elevTop_ + gy);

            // Pull the grid point back into the diamond. sub_6B2A70 applies the
            // three offsets to the ORIGINAL coords, in this order, and gives up
            // after them (0x6b2bb5 - 0x6b2be3).
            static const int16_t kOffX[3] = {  0, -1, -1 };
            static const int16_t kOffY[3] = { -1, -1,  0 };
            int16_t cx = bx;
            int16_t cy = by;
            for (int step = 0; !CellExists(cx, cy) && step < 3; ++step)
            {
                cx = static_cast<int16_t>(bx + kOffX[step]);
                cy = static_cast<int16_t>(by + kOffY[step]);
            }

            if (!CellExists(cx, cy))
            {
                entry.corner = 0;                    // v39 was never set
                entry.blocked = 1;                   // 0x6b2ee4
                continue;
            }

            // The seed value comes from the cell the walk landed on (0x6b2c95).
            const MapCell* seed = CellAt(cx, cy);
            const unsigned int slope =
                static_cast<unsigned int>(seed->SlopeIndex);
            entry.corner = 15 * seed->Level
                         + (slope < 19 ? kSlopeCorners[slope][0] : 0);

            // The four probes are all run on the ORIGINAL coords (0x6b2ca3 -
            // 0x6b2eda): (bx, by - 1), (bx - 1, by - 1), (bx - 1, by), (bx, by).
            // Each is a plain "in the diamond and unusable" test; an
            // out-of-diamond probe sets nothing.
            if ((CellExists(bx, static_cast<int16_t>(by - 1))
                     && !CanHostSlope(bx, static_cast<int16_t>(by - 1)))
             || (CellExists(static_cast<int16_t>(bx - 1), static_cast<int16_t>(by - 1))
                     && !CanHostSlope(static_cast<int16_t>(bx - 1),
                                      static_cast<int16_t>(by - 1)))
             || (CellExists(static_cast<int16_t>(bx - 1), by)
                     && !CanHostSlope(static_cast<int16_t>(bx - 1), by))
             || (CellExists(bx, by) && !CanHostSlope(bx, by)))
            {
                entry.blocked = 1;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Step 4 - ApplyElevationLevels (the inline loop at 0x5A361A - 0x5A36B7).
//
// For every work slot whose packed coords are not (0,0):
//     target  = (double)cell->Level + work.double(+8)   // the 0x5A3652/56 reads
//     current = sub_6B4100(cell)                        // the grid's own level
//     d       = F2I64(target - current);  step = (d >= 0) ? 1 : -1
//     for (n = |d|; n; --n)
//     {
//         v = sub_6B4240(cell, step);                   // corners to move
//         sub_6B3E60(coords, v);                        // apply one step
//     }
//
// The three helpers each bring their own constraints: sub_6B4100 answers
// min(corner of the 2x2 block) / 15 while the cell is morphable and its +69
// is clear, otherwise the cell's own Level byte; sub_6B4240 builds the 4-bit
// corner mask (all-equal -> the untouched corners, else the corners below the
// max for a raise / above the min for a lower); sub_6B3E60 writes 15 * delta
// into the masked corners, keeps only values in [0, 180] and journals every
// accepted write so a rejected step (or a failed sub_6B3A80 check) can be
// rolled back.
// ---------------------------------------------------------------------------
void RandomMapGenerator::ApplyElevationLevels()
{
    if (workCells_ == nullptr || cellSlots_ == nullptr || elevGrid_.empty())
        return;

    const int side = size_.workSide;
    const int count = side * side;

    for (int i = 0; i < count; ++i)
    {
        const int packed = workCells_[i].MapCoords();      // 0x5a3629
        if (packed == 0)                                   // 0x5a3643 gate
            continue;

        const int16_t x = static_cast<int16_t>(packed & 0xFFFF);
        const int16_t y = static_cast<int16_t>(
                              static_cast<uint32_t>(packed) >> 16);
        const MapCell* cell = CellAt(x, y);

        // target = (double)cell->Level + the rounded height field
        // (0x5a3652 - 0x5a3659; the cell's Level is the byte at +0x11B).
        const double target = static_cast<double>(cell->Level)
                            + *reinterpret_cast<const double*>(
                                  workCells_[i].data + 2);
        const int current = ElevationCurrentLevel(cell);   // 0x5a365d
        const int diff = F2I64(target - static_cast<double>(current)); // 0x5a366e
        const int step = (diff >= 0) ? 1 : -1;             // 0x5a367e - 0x5a3688

        // 0x5a368a - 0x5a36a5: |diff| steps, each raising or lowering the cell
        // by one level.
        for (int n = (diff < 0 ? -diff : diff); n > 0; --n)
        {
            const int mask = ElevationStepMask(cell, step);   // 0x5a3692
            ApplyElevationStep(step, x, y, mask);             // 0x5a369f
        }
    }
}

// ---------------------------------------------------------------------------
// The grid accessors.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::ElevationIndexInRange(int index) const
{
    return index >= 0 && index < static_cast<int>(elevGrid_.size());
}

int RandomMapGenerator::ElevationCornerAt(int index) const
{
    // Guard only: for every cell of the diamond the four indices a cell uses
    // (index, index + 1, index + stride, index + stride + 1) stay inside the
    // rectangle that covers the iterator's whole span. The vanilla indexes
    // blindly.
    if (!ElevationIndexInRange(index))
        return 0;
    return elevGrid_[index].corner;
}

void RandomMapGenerator::RollbackElevationJournal()
{
    // 0x6b4056 - 0x6b4075: the journal is replayed backwards.
    for (size_t i = elevJournal_.size(); i > 0; --i)
    {
        const ElevationWrite& w = elevJournal_[i - 1];
        if (ElevationIndexInRange(w.index))
            elevGrid_[w.index].corner = w.oldCorner;
    }
    elevJournal_.clear();
}

// ---------------------------------------------------------------------------
// ElevationCurrentLevel - sub_6B4100 (0x6B4100 - 0x6B4231).
//
// The cell's level according to the grid: the smallest of the four corners of
// its 2x2 block, divided by 15. When the cell cannot host a slope the cell's own
// Level is answered instead - and the vanilla's condition there (in the diamond,
// no overlay, no object, work +69 clear, morphable tile) is exactly
// CanHostSlope, so the two collapse into one test.
// ---------------------------------------------------------------------------
int RandomMapGenerator::ElevationCurrentLevel(const MapCell* cell)
{
    const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int16_t y = static_cast<int16_t>(
                          static_cast<uint32_t>(cell->MapCoords) >> 16);
    if (!CanHostSlope(x, y))
        return cell->Level;                              // 0x6b4207 (+0x11B)

    const int index = (x - elevLeft_) + elevStride_ * (y - elevTop_);
    int lowest = 1000;                                   // 0x6b4162 literal seed
    const int corners[4] = { index, index + 1,
                             index + elevStride_ + 1, index + elevStride_ };
    for (int k = 0; k < 4; ++k)
    {
        const int corner = ElevationCornerAt(corners[k]); // 0x6b4144 - 0x6b415e
        if (corner < lowest)
            lowest = corner;
    }
    return lowest / 15;                                  // 0x6b421e
}

// ---------------------------------------------------------------------------
// ElevationStepMask - sub_6B4240 (0x6B4240 - 0x6B43AC).
//
// The four corners in this function's order are
//     e(x, y), e(x + 1, y), e(x + 1, y + 1), e(x, y + 1)
// (bit 0 .. bit 3 of the result). A corner is set when it is not blocked and
//     - all four corners equal, or
//     - step <= 0 (lowering) and the corner is above the minimum, or
//     - step > 0  (raising) and the corner is below the maximum.
// ---------------------------------------------------------------------------
int RandomMapGenerator::ElevationStepMask(const MapCell* cell, int step)
{
    const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
    const int16_t y = static_cast<int16_t>(
                          static_cast<uint32_t>(cell->MapCoords) >> 16);
    const int indexA = (x - elevLeft_) + elevStride_ * (y - elevTop_);
    const int indexB = indexA + elevStride_;             // the row below

    const int corners[4] = { indexA, indexA + 1, indexB + 1, indexB };

    int lowest = 1000;                                   // 0x6b42f5
    int highest = -1;                                    // 0x6b42fa
    for (int k = 0; k < 4; ++k)
    {
        const int corner = ElevationCornerAt(corners[k]);
        if (corner < lowest)
            lowest = corner;
        if (corner > highest)
            highest = corner;
    }

    int result = 0;
    for (int k = 0; k < 4; ++k)
    {
        if (ElevationIndexInRange(corners[k]) && elevGrid_[corners[k]].blocked)
            continue;                                    // blocked corners never move

        bool setBit;
        if (highest == lowest)
            setBit = true;                               // 0x6b4320 - 0x6b433b
        else if (step <= 0)
            setBit = ElevationCornerAt(corners[k]) > lowest;   // 0x6b434b
        else
            setBit = ElevationCornerAt(corners[k]) < highest;  // 0x6b437d
        if (setBit)
            result |= 1 << k;
    }
    return result;
}

// ---------------------------------------------------------------------------
// ValidateElevationStep - sub_6B3A80 (0x6B3A80 - 0x6B3CC1).
//
// Walks the eight grid neighbours of (x, y) - the centre is stepped over - and
// enforces that no neighbour's corner ends up more than 15 (one level) away from
// the reference corner: a neighbour further off in the step's direction is
// pulled to exactly one level away and then validated recursively. Every write
// goes into the caller's journal.
//
// The vanilla takes both the coordinates and the bound checks in grid-relative
// space; the port keeps map coordinates and converts.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::ValidateElevationStep(int delta, int16_t x, int16_t y)
{
    const int base = (x - elevLeft_) + elevStride_ * (y - elevTop_);
    if (!ElevationIndexInRange(base))
        return false;
    const int reference = elevGrid_[base].corner;         // v7 / v19

    // The disassembly's neighbour order (0x6b3aa3 starts the pair at dx = dy =
    // -1 and walks -1, 0, 1 twice): (-1,-1), (0,-1), (1,-1), (-1,0),
    // [(0,0) skipped], (1,0), (-1,1), (0,1), (1,1). All EIGHT neighbours take
    // part - the north-west one is the first checked, not an omission.
    static const int16_t kDX[8] = { -1,  0,  1, -1,  1, -1,  0,  1 };
    static const int16_t kDY[8] = { -1, -1, -1,  0,  0,  1,  1,  1 };
    const int rows = elevStride_ + 1;

    for (int k = 0; k < 8; ++k)
    {
        const int16_t nx = static_cast<int16_t>(x + kDX[k]);
        const int16_t ny = static_cast<int16_t>(y + kDY[k]);
        const int gx = nx - elevLeft_;
        const int gy = ny - elevTop_;
        if (gx < 0 || gy < 0 || gx >= elevStride_ || gy >= rows)
            return false;                                // 0x6b3c8d (flag == 0)
        const int index = gx + elevStride_ * gy;
        if (!ElevationIndexInRange(index))
            return false;

        ElevationEntry& entry = elevGrid_[index];

        int distance = entry.corner - reference;
        if (distance < 0)
            distance = -distance;

        if (entry.blocked)
        {
            if (distance > 15)
                return false;                            // 0x6b3b55
            continue;
        }
        if (distance <= 15)
            continue;                                    // 0x6b3b6e

        if (delta == 1)
        {
            if (entry.corner < reference)                // 0x6b3b79
            {
                elevJournal_.push_back(
                    ElevationWrite{ index, entry.corner }); // 0x6b3bd9
                entry.corner = reference - 15;           // 0x6b3c5b
                entry.touched = 1;
            }
        }
        else if (entry.corner > reference)               // 0x6b3bf0
        {
            elevJournal_.push_back(
                ElevationWrite{ index, entry.corner });  // 0x6b3c48
            entry.corner = reference + 15;               // 0x6b3c58
            entry.touched = 1;
        }

        if (entry.blocked || !ValidateElevationStep(delta, nx, ny))
            return false;                                // 0x6b3c87
    }
    return true;                                         // 0x6b3cbc
}

// ---------------------------------------------------------------------------
// ApplyElevationStep - sub_6B3E60 (0x6B3E60 - 0x6B40FC).
//
// Adds 15 * delta to the masked corners of the 2x2 grid block at (x, y). Each
// write is journalled first and kept only while the new value stays in
// [0, 180]; every touched point is validated (ValidateElevationStep) and a
// failure - like a blocked point, or a grid point that leaves the block - rolls
// the whole journal back and fails the step.
//
// The driver always calls this with the flag = 0 (0x5a369a `xor dl, dl`), which
// is why the vanilla's third journal byte and its "keep going on a blocked
// point" branch are both dead here.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::ApplyElevationStep(int delta, int16_t x, int16_t y,
                                            unsigned int mask)
{
    elevJournal_.clear();                                // 0x6b3e7a

    // sub_6B4240 hands the mask over in its own corner order - e(x,y),
    // e(x+1,y), e(x+1,y+1), e(x,y+1) - while this function walks the 2x2 block
    // row by row: e(x,y), e(x+1,y), e(x,y+1), e(x+1,y+1). The disassembly swaps
    // the two middle bits for that (0x6b3ebd).
    const unsigned int walkMask = (mask & 3u)
                                | (2u * (mask & 4u))
                                | ((mask >> 1) & 4u);

    for (int k = 0; k < 4; ++k)
    {
        if ((walkMask & (1u << k)) == 0)
            continue;

        const int16_t nx = static_cast<int16_t>(x + (k & 1));
        const int16_t ny = static_cast<int16_t>(y + ((k >> 1) & 1));
        const int index = (nx - elevLeft_) + elevStride_ * (ny - elevTop_);
        if (!ElevationIndexInRange(index))
            continue;

        ElevationEntry& entry = elevGrid_[index];

        if (entry.blocked)
        {
            // 0x6b4089: with the driver's flag = 0 a blocked point aborts the
            // step and undoes everything written so far.
            RollbackElevationJournal();
            return false;
        }

        const int original = entry.corner;
        entry.corner = 15 * delta + original;            // 0x6b3f42
        if (entry.corner < 0 || entry.corner > 180)      // 0x6b3f4f
            entry.corner = original;                     // 0x6b3fe8: undone in place
        else
            elevJournal_.push_back(
                ElevationWrite{ index, original });      // 0x6b3faf
        entry.touched = 1;                               // 0x6b3fcf

        if (!ValidateElevationStep(delta, nx, ny))       // 0x6b3fe2
        {
            RollbackElevationJournal();
            return false;
        }
    }

    elevJournal_.clear();                                // 0x6b40bc
    return true;
}

// ---------------------------------------------------------------------------
// Step 5 - FinalizeElevationSlopes (sub_6B3850, 0x6B3850 - 0x6B3A7C).
//
// Walks the diamond; for a cell whose 2x2 grid block was touched, with
// max - min <= 15 and the cell a valid target (diamond, no overlay, no object,
// work byte +69 clear, morphable tile):
//     cell->Level = min / 15
//     the four corners -= min                       // the normalized pattern
//     SlopeIndex   = the pattern's index in off_83FF18 (0..18)
//     IsoTileTypeIndex = SlopeIndex ? rampBaseIndex_ + SlopeIndex - 1
//                                   : clearTileIndex_
// then frees the grid.
// ---------------------------------------------------------------------------
void RandomMapGenerator::FinalizeElevationSlopes()
{
    if (cellSlots_ == nullptr || elevGrid_.empty())
        return;

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);                 // 0x6b3858
    while (MapCell* cell = it.Next())
    {
        const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t y = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);
        const int indexA = (x - elevLeft_) + elevStride_ * (y - elevTop_);
        const int indexB = indexA + elevStride_;          // the row below
        const int index[4] = { indexA, indexA + 1, indexB + 1, indexB };

        // 0x6b38c7: only cells whose 2x2 block has a touched grid point are
        // considered at all.
        bool touched = false;
        for (int k = 0; k < 4 && !touched; ++k)
            touched = ElevationIndexInRange(index[k])
                   && elevGrid_[index[k]].touched != 0;
        if (!touched)
            continue;

        // The four corners, in this function's order (0x6b38db - 0x6b3913):
        // e(x,y), e(x + 1,y), e(x + 1,y + 1), e(x,y + 1).
        int corner[4];
        int lowest = 1000;                                // 0x6b38f6
        int highest = 0;                                  // 0x6b38fb
        for (int k = 0; k < 4; ++k)
        {
            corner[k] = ElevationCornerAt(index[k]);
            if (corner[k] < lowest)
                lowest = corner[k];
            if (corner[k] > highest)
                highest = corner[k];
        }
        if (highest - lowest > 15)                        // 0x6b391a
            continue;                                     // more than one level

        if (!CanHostSlope(x, y))                          // 0x6b396f - 0x6b39b2
            continue;

        // Port tuning (user reports 2026-10-01): refuse a hill micro-slope
        // that shares an EDGE with a CliffSet facade two or more levels higher
        // unless that edge already holds a same-level CliffSet foot cell
        // (whose art renders the intervening wall). Only ORTHOGONAL neighbours
        // count: diagonal L4-slope/L8-cliff touches are a vanilla-legal
        // configuration (verified on the reference map, e.g. t34 L4 next to
        // t50 L8 on the NE corner) and suppressing those deleted a legitimate
        // connecting band slope at (90,121) in rmg_085817.
        {
            const int newLevel = lowest / 15;
            static const int16_t oDX[4] = { 0, 1, 0, -1 };
            static const int16_t oDY[4] = { -1, 0, 1, 0 };
            bool block = false;
            for (int sd = 0; sd < 4; ++sd)
            {
                const MapCell* oq = CellAt(
                    static_cast<int16_t>(x + oDX[sd]),
                    static_cast<int16_t>(y + oDY[sd]));
                if (oq != nullptr
                    && oq->IsoTileTypeIndex >= shoreTileIndex_
                    && oq->IsoTileTypeIndex <= shoreTileIndex_ + 39
                    && oq->Level - newLevel >= 2)
                {
                    block = true;
                    break;
                }
            }
            if (block)
            {
                bool sameLevelFoot = false;
                for (int sd = 0; sd < 4; ++sd)
                {
                    const MapCell* sq = CellAt(
                        static_cast<int16_t>(x + oDX[sd]),
                        static_cast<int16_t>(y + oDY[sd]));
                    if (sq != nullptr
                        && sq->IsoTileTypeIndex >= shoreTileIndex_
                        && sq->IsoTileTypeIndex <= shoreTileIndex_ + 39
                        && sq->Level == newLevel)
                    {
                        sameLevelFoot = true;
                        break;
                    }
                }
                if (!sameLevelFoot)
                    continue;
            }
        }

        // 0x6b39dc: the level. Written BEFORE the pattern lookup, so a cell whose
        // corner pattern is not in the table still has its level updated - only
        // the slope and the tile are skipped for it.
        cell->Level = lowest / 15;

        for (int k = 0; k < 4; ++k)                       // 0x6b39ea
            corner[k] -= lowest;                          // the normalized pattern

        // 0x6b39f9: find that pattern in off_83FF18 (0..18; running past the
        // end leaves the cell with just its new level).
        int slope = -1;
        for (int k = 0; k < 19; ++k)
        {
            if (kSlopeCorners[k][0] == corner[0]
             && kSlopeCorners[k][1] == corner[1]
             && kSlopeCorners[k][2] == corner[2]
             && kSlopeCorners[k][3] == corner[3])
            {
                slope = k;
                break;
            }
        }
        if (slope < 0)
            continue;                                     // 0x6b3a25 -> LABEL_36

        cell->SlopeIndex = slope;                         // 0x6b3a2b
        cell->IsoTileTypeIndex = slope
            ? (rampBaseIndex_ + slope - 1)                // 0x6b3a40 (0xABC1D8)
            : clearTileIndex_;                            // 0x6b3a33 (0xAA10B0)
    }

    // 0x6b3a68: the scratch grid is freed and dword_B0B6EC cleared.
    elevGrid_.clear();
}

// ---------------------------------------------------------------------------
// Step 6 - MergeSlopeBlocks (the inline merge at 0x5A36CB - 0x5A38A9).
//
// Walks the diamond and, for a cell whose SlopeIndex is 5 or 11, checks the
// 2x2 block it anchors - the four cells in order (0,0) (1,0) (1,1) (0,1):
//     SlopeIndex == 5  -> they must be 5 / 6 / 7 / 8
//     SlopeIndex == 11 -> they must be 11 / 12 / 9 / 10
// When all four match, each of the four cells is stamped
//     IsoTileTypeIndex = 0xFFFF, Height = 0, SlopeIndex = 0
// and, only for the 11 case, Level += 1.
// ---------------------------------------------------------------------------
void RandomMapGenerator::MergeSlopeBlocks()
{
    if (cellSlots_ == nullptr)
        return;

    // The four cells of the block, in the vanilla's packed-offset order
    // v33 = { (0,0), (1,0), (1,1), (0,1) } (0x5a36f1 - 0x5a3729).
    static const int16_t kOffX[4] = { 0, 1, 1, 0 };
    static const int16_t kOffY[4] = { 0, 0, 1, 1 };
    // v34 (0x5a372d - 0x5a3745): the slope each cell must carry when the
    // anchor's slope is 11. For a slope-5 anchor the expected values are simply
    // 5, 6, 7, 8 in the same order.
    static const int kExpected11[4] = { 11, 12, 9, 10 };

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);                 // 0x5a36cb
    while (MapCell* cell = it.Next())
    {
        const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t y = static_cast<int16_t>(
                              static_cast<uint32_t>(cell->MapCoords) >> 16);
        const int anchor = cell->SlopeIndex;              // 0x5a375a

        // Only the two anchor slopes take part; everything else is skipped
        // (0x5a3769 / 0x5a37c9 / LABEL_24). The 11 case also bumps the level.
        int bump;
        if (anchor == 5)
            bump = 0;
        else if (anchor == 11)
            bump = 1;
        else
            continue;

        // The block merges only when all four cells carry the expected slope
        // (0x5a37b5 for the 5 case, 0x5a3816 for the 11 case). The vanilla keeps
        // scanning after a mismatch - only the flag is cleared - so this loop
        // does too.
        bool allMatch = true;                             // v25
        for (int k = 0; k < 4; ++k)
        {
            const int expected = (anchor == 5) ? (k + 5) : kExpected11[k];
            if (CellAt(static_cast<int16_t>(x + kOffX[k]),
                       static_cast<int16_t>(y + kOffY[k]))->SlopeIndex != expected)
                allMatch = false;
        }
        if (!allMatch)                                    // LABEL_21
            continue;

        // Port tuning (user report 2026-10-01): do not collapse a slope block
        // when it sits right against a carved region ramp (CliffRamps
        // 384-393) or a CliffSet facade outside the block. Vanilla's saved
        // maps never carry a merged 0xFFFF cell on such a seam; collapsing
        // here stamped empty cells beside the ramp and left white wall
        // triangles (e.g. rmg_090407 near the 386/387 strips). On open ground
        // the merge proceeds exactly as vanilla.
        {
            bool touchesArt = false;
            for (int k = 0; k < 4 && !touchesArt; ++k)
            {
                const int16_t bx = static_cast<int16_t>(x + kOffX[k]);
                const int16_t by = static_cast<int16_t>(y + kOffY[k]);
                for (int adx = -1; adx <= 1 && !touchesArt; ++adx)
                {
                    for (int ady = -1; ady <= 1; ++ady)
                    {
                        if (adx == 0 && ady == 0)
                            continue;
                        const int16_t ax = static_cast<int16_t>(bx + adx);
                        const int16_t ay = static_cast<int16_t>(by + ady);
                        // ignore the other three cells of this very block
                        if (ax >= x && ax <= x + 1 && ay >= y && ay <= y + 1)
                            continue;
                        const MapCell* aq = CellAt(ax, ay);
                        if (aq == nullptr)
                            continue;
                        const int at = aq->IsoTileTypeIndex;
                        if ((at >= cliffRampsIndex_ && at <= cliffRampsIndex_ + 9)
                            || (at >= shoreTileIndex_
                                && at <= shoreTileIndex_ + 39))
                        {
                            touchesArt = true;
                            break;
                        }
                    }
                }
            }
            if (touchesArt)
                continue;
        }

        // Collapse the four cells into the multi-cell tile (0x5a382b - 0x5a3891):
        // 0xFFFF is the "no own tile" mark the delta corners use too.
        for (int k = 0; k < 4; ++k)
        {
            MapCell* c = CellAt(static_cast<int16_t>(x + kOffX[k]),
                                static_cast<int16_t>(y + kOffY[k]));
            const int level = c->Level;                   // 0x5a386a
            c->IsoTileTypeIndex = 0xFFFF;                 // 0x5a3876
            c->Height = 0;                                // 0x5a387d
            c->SlopeIndex = 0;                            // 0x5a3884
            c->Level = bump + level;                      // 0x5a388b
        }
    }
}

// ---------------------------------------------------------------------------
// RemoveCliffsOverRamps - port-only final sweep.
//
// PlaceCliffPiece's pre-stamp guard can only see ramps that exist while cliffs
// are placed. FinalizeElevationSlopes runs later (this stage) and lays micro
// slope tiles (RampBase + slope - 1) from the final elevation corner pattern,
// so a CliffSet piece accepted earlier can finish with a wall cell riding a
// freshly laid ramp body.
//
// Group the surviving CliffSet cells back into pieces (a piece's sub-cell
// Height is col + row*CellsInX, so the anchor inverts with modulo / divide),
// find the riding wall cells (IsCliffWallRiding), erase the piece and stamp
// smaller CliffSet art on the surviving cells instead of leaving bare ground:
//   - 2x2 walls (slots 4-7) whose east column rides collapse to the C8 1x2
//     vertical strip on the west column;
//   - every other surviving wall cell becomes C34 (t82), surviving feet one
//     of C12..C14;
//   - only the riding cells themselves go back to t0 (the ramp corridor the
//     game loader fills).
// Ramp-top seam pieces (a wall met at its own level by a ramp top cell) are
// left untouched - their facade is required art.
// ---------------------------------------------------------------------------
void RandomMapGenerator::RemoveCliffsOverRamps()
{
    if (cellSlots_ == nullptr || shoreTileIndex_ < 0)
        return;

    struct Member { int16_t x, y; int h, level; };
    struct Piece { int tile; int16_t ax, ay; std::vector<Member> cells; };
    std::vector<Piece> pieces;

    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())
    {
        const int t = cell->IsoTileTypeIndex;
        if (t < shoreTileIndex_ || t >= shoreTileIndex_ + 40)
            continue;
        const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);
        const int w = CliffCellsInX(t);
        const int ax = x - cell->Height % w;
        const int ay = y - cell->Height / w;

        Piece* found = nullptr;
        for (Piece& pc : pieces)
        {
            if (pc.tile == t && pc.ax == ax && pc.ay == ay)
            {
                found = &pc;
                break;
            }
        }
        if (found == nullptr)
        {
            pieces.push_back(Piece{ t,
                                    static_cast<int16_t>(ax),
                                    static_cast<int16_t>(ay), {} });
            found = &pieces.back();
        }
        found->cells.push_back(Member{ x, y, cell->Height, cell->Level });
    }

    const int tVStrip   = shoreTileIndex_ + 7;    // C8
    const int tWallCap  = shoreTileIndex_ + 33;   // C34
    const int tFlatBase = shoreTileIndex_ + 11;   // C12..14

    DiagLog("UNWALL-START pieces=%zu shoreBase=%d slopeSetBase=%d",
            pieces.size(), shoreTileIndex_, slopeSetPiecesIndex_);
    for (Piece& pc : pieces)
    {
        const int slot = pc.tile - shoreTileIndex_ + 1;
        std::vector<const Member*> riders;
        for (const Member& m : pc.cells)
        {
            if (m.level >= 7 && IsCliffWallRiding(m.x, m.y, m.level))
                riders.push_back(&m);
        }
        if (riders.empty())
            continue;

        // Take the piece off first; only the non-riding members get new art.
        for (const Member& m : pc.cells)
        {
            MapCell* c = CellAt(m.x, m.y);
            if (c != nullptr)
            {
                DiagLog("CLIFF-UNWALL t%d anchor=(%d,%d) cell=(%d,%d) L%d",
                        pc.tile, pc.ax, pc.ay, m.x, m.y, m.level);
                c->IsoTileTypeIndex = 0;
                c->Height = 0;
            }
        }

        const auto isRider = [&](int h) {
            for (const Member* r : riders)
                if (r->h == h) return true;
            return false;
        };
        const auto memberAt = [&](int h) -> const Member* {
            for (const Member& m : pc.cells)
                if (m.h == h) return &m;
            return nullptr;
        };
        const auto restamp = [&](int x, int y, int tile, int height, int level)
        {
            MapCell* c = CellAt(static_cast<int16_t>(x),
                                static_cast<int16_t>(y));
            if (c == nullptr || c->IsoTileTypeIndex != 0)
                return;                                   // ramp / other art
            DiagLog("CLIFF-REWALL t%d (%d,%d) -> t%d/h%d L%d",
                    pc.tile, x, y, tile, height, level);
            c->IsoTileTypeIndex = tile;
            c->Height = static_cast<uint8_t>(height);
            c->Level = level;
        };

        // C8: west column h0 wall + h2 foot survives, east column rides.
        const Member* westTop  = memberAt(0);
        const Member* westFoot = memberAt(2);
        if (slot >= 4 && slot <= 7
            && westTop != nullptr && westTop->level >= 7 && !isRider(0)
            && westFoot != nullptr && westFoot->level <= 5 && !isRider(2))
        {
            restamp(westTop->x, westTop->y, tVStrip, 0, westTop->level);
            restamp(westFoot->x, westFoot->y, tVStrip, 1, westFoot->level);
            continue;
        }

        for (const Member& m : pc.cells)
        {
            if (isRider(m.h))
                continue;                                   // stays t0
            if (m.level >= 7)
                restamp(m.x, m.y, tWallCap, 0, m.level);
            else
                restamp(m.x, m.y,
                        tFlatBase + (pc.ax + pc.ay + m.h) % 3, 0, m.level);
        }

        // Riding wall cells keep tile=0, but their Level was bumped by the
        // wall's z=4 during placement. Restore the pre-wall ground level so a
        // lowered cliff-base cell does not get stranded at the wall height.
        for (const Member* r : riders)
        {
            MapCell* c = CellAt(r->x, r->y);
            if (c != nullptr && c->IsoTileTypeIndex == 0)
                c->Level = r->level - 4;
        }
    }
}
