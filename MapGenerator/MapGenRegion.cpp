// ============================================================================
// MapGenRegion.cpp - region subsystem of the random map generator
//
//   InitRegions                sub_598960 @ 0x598C24   stage body
//   ReleaseRegionObject        sub_5AC290   region object removal + delete
//   CreateRegionRecord         sub_58BF70   region record constructor
//   SeedRegionsFromWaterCells  sub_58CF90   seed regions from water / green cells
//   BuildRegionFromCell        sub_58C800   cell -> region record builder
//   GrowRegionBody             sub_58E740   region body build (fan-out)
//   CollectRegionBoundary      sub_58D410   the region's boundary cells
//   SweepRegionBody            sub_58E9B0   region body build (worklist sweep)
//   AssignRemainingCells       sub_58D010   assign the leftover cells
//
// The stage is the "RMG: Init regions" debug point of the RMG main flow; the
// following "Making regions" stage (region merge, ramps, cliffs) will land here
// too. Decompiles: 598C24_InitRegions.c and the files it lists (58CF90.c,
// 58D010.c, 5AC290.c, 58E740.c, 58E9B0.c, 58C800.c, 58D410.c, 58BF70.c,
// 4867B0.c, 5AC370.c, 42F7C0.c) in the project's decompile folder.
// ============================================================================

#include "pch.h"
#include "MapGen.h"

// ---------------------------------------------------------------------------
// The global `Level` (@0xABE2E4) that sub_58C800's two reset paths fall back on
// when the saved cell Level is -1 ("Level_1 = ::Level; if (Level != -1)
// Level_1 = Level;" at 0x58cdd0 and 0x58cc4a).
//
// It is NOT an unwritten zero. 0xABE2E4 is the RMG instance's this[195]
// (0xABDFD8 + 4*195) - the port's baseLevel_. xrefs_to lists only the two reads
// above because sub_599650 writes it with a REGISTER-relative store:
// 0x59976e and 0x5997ae "mov [edi+30Ch], esi" with esi = 4. The river's canyon
// path then raises it by 4 (0x59e512), and Init-regions runs AFTER that canyon
// pass - so the value read here is the live base level, 4 or 8.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// InitRegions - the "RMG: Init regions" stage of sub_598960, 0x598C24 ..
// 0x598D42 (the debug string 0x82BED8 is referenced only from 0x598C24, and the
// stage ends right before the "Making regions" block at 0x598D44).
//
// NOT gated by land type - every map runs it, straight after the water-detail
// pass. The only land-type dependency sits in step 5 (the GrowRegionBody pass
// count). Six steps:
//
//   1. reset the work array region marks over the whole work square:
//        work[+56] = -1   (byte 56 = data[14], the region mark)
//        work[+60] = -1   (byte 60 = data[15], the visited mark)
//   2. tear the previous region objects down, newest first
//   3. region-id counter reset (vanilla dword_ABED14 = this->regionIdCounter_)
//   4. seed regions from the water-family / green-ground cells
//   5. for every region seeded in step 4 whose water flag is set, run the body
//      build with a pass count of (cellCount > 8000) + !inlandOrMountain + 4,
//      then the worklist sweep
//   6. assign the cells step 4 left unassigned
//
// Note: step 2's array shrink happens inside ReleaseRegionObject (vanilla
// sub_5AC290 decrements the global count), which erases the entry it is given -
// the backward index walk stays valid because an erased slot only shifts the
// entries above it, and every release call takes the entry at the current
// index, whose pointer is unique in the array.
// ---------------------------------------------------------------------------
void RandomMapGenerator::InitRegions()
{
    // ---- 1. Reset the work region marks (0x598c38 - 0x598c64) -------------
    if (workCells_)
    {
        const int workCount = size_.workSide * size_.workSide;   // 0x598c40
        for (int i = 0; i < workCount; ++i)                      // 0x598c47
        {
            workCells_[i].data[14] = -1;                         // 0x598c52 (byte 56)
            workCells_[i].data[15] = -1;                         // 0x598c5c (byte 60)
        }
    }

    // ---- 2. Tear the previous region objects down (0x598c71) -------------
    for (int i = static_cast<int>(regions_.size()) - 1; i >= 0; --i)
    {
        if (regions_[i])
            ReleaseRegionObject(regions_[i], true);              // 0x598c81
    }

    // ---- 3. Region-id counter reset (0x598c89) ---------------------------
    regionIdCounter_ = 0;

    // ---- 4. Seed regions from the water-family / green-ground cells ------
    // (0x598c8f)
    SeedRegionsFromWaterCells();

    // ---- 5. Grow the body of every water region (0x598c9d - 0x598cfa) ----
    // isInlandOrMountain is the stage's only land-type dependency: it turns the
    // pass count into 4 (inland / mountainous) or 5 (everything else), plus one
    // more when the region covers more than 8000 cells.
    const int  landType = static_cast<int>(config_.landType);
    const bool inlandOrMountain = (landType == 3 || landType == 4);  // 0x598cc7

    for (size_t j = 0; j < regions_.size(); ++j)                 // 0x598c9d
    {
        MapRegion* region = regions_[j];
        if (region == nullptr)
            continue;
        if (!region->waterFamily)                                // 0x598cad (byte +0x14)
            continue;

        const int passes = (region->cellCount > 8000)            // 0x598ce4
                         + (inlandOrMountain ? 0 : 1) + 4;
        GrowRegionBody(region, passes);
        SweepRegionBody(region);                                 // 0x598ceb
    }

    // ---- 6. Assign the leftover cells (0x598cfc) -------------------------
    AssignRemainingCells(0);                                     // 0x598cfa/0x598cfc
}

// ---------------------------------------------------------------------------
// Init-regions stage subordinates.
//
// Each mirrors one vanilla routine; InitRegions calls them in the vanilla
// order. The full decompiles live in the project's decompile folder (see the
// file list in this file's header comment). All of them are landed.
// ---------------------------------------------------------------------------

// sub_5AC290 - remove one region object from the global region array
// (dword_ABDF94 / count dword_ABDFA0) and, when `freeObject` is set, delete it.
//
// Vanilla body, step by step (5AC290.c):
//   1. v3 = *obj (+0, the object's own vftable pointer); when set, run its
//      virtual destructor (**v3)(v3, 1) and clear the slot. Region objects come
//      from sub_58BF70, which stores 0 there (0x58bf7a), so for every object the
//      Init-regions stage owns this step is dead; MapRegion is a plain struct
//      with no vftable, so it is not modelled at all.
//   2. FindIndex(obj) through the owning array class (dword_ABDF90 vtable +16);
//      when the index is valid and in range, --count and shift the tail down one
//      slot (0x5ac2ba - 0x5ac2ed). Not found -> the count is left alone and only
//      steps 3 / 4 run.
//   3. tear the embedded DynamicVectorClass<Cell> at +40 down (0x5ac2ef -
//      0x5ac31e): install the base VectorClass<Cell> vftable at +40, operator
//      delete the Items buffer (+44) when both it and the "allocated" byte (+53)
//      are set, then clear the byte (+53) and the capacity (+48). In this port
//      the holder is the MapRegion::cells vector (+0x28), so its own destructor
//      does exactly that - nothing to free by hand.
//   4. a2 & 1 -> operator delete(obj) (0x5ac325 - 0x5ac328).
//
// Vanilla returns the object pointer; every call site (the stage teardown loop,
// 0x598c81) discards it, so the result is dropped here.
void RandomMapGenerator::ReleaseRegionObject(MapRegion* region, bool freeObject)
{
    if (region == nullptr)
        return;

    // ---- 2. array removal + tail compaction ------------------------------
    // Vanilla searches by pointer identity (FindIndex) and shifts the tail down
    // one slot; std::vector::erase is that same compaction. When the object is
    // not in the array nothing happens to the count - exactly as vanilla - and
    // only the delete below runs.
    for (size_t i = 0; i < regions_.size(); ++i)
    {
        if (regions_[i] != region)
            continue;
        regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }

    // ---- 3. embedded cell vector teardown ---------------------------------
    // Nothing to free by hand: MapRegion::cells owns the buffer and the object's
    // destructor (the delete below) releases it - the same thing vanilla's
    // vtable-install + operator delete pair does.

    // ---- 4. delete the object when the caller asked for it ---------------
    if (freeObject)
        delete region;                                            // 0x5ac328
}

// sub_58CF90 - seed the regions: linear sweep over the work square, and for
// every cell with an unused region mark that carries a water-family tile
// (sub_4865D0) or a green-ground tile (sub_4867B0), build its region record
// through sub_58C800.
//
// Vanilla body (58CF90.c, 0x58cf96 - 0x58d008):
//   count = workSide * workSide;                      // 0x58cf96
//   for (i = 0; i < count; ++i, off += 80)            // work cell at work[off]
//   {
//       packed = work[off].MapCoords;                 // work[+0], the CellStruct
//       cell   = MapClass::GetCellAt_MapCrd(packed);  // 0x58cfbf, fetched first
//       if (work[off + 56] != -1) continue;           // 0x58cfd0: mark in use
//       if (!work[off] && !work[off + 2]) continue;   // 0x58cfd4: packed coords
//                                                     // zero -> no diamond cell
//                                                     // was ever placed here
//       if (sub_4865D0(cell) || sub_4867B0(cell))     // 0x58cfe9
//           sub_58C800(cell);                         // 0x58cff4
//   }
// The incoming argument slot is dead - vanilla overwrites it with the first
// work-cell coords before reading it - so the routine takes no parameters, and
// its return value (the work-array base) is ignored.
void RandomMapGenerator::SeedRegionsFromWaterCells()
{
    if (workCells_ == nullptr)
        return;

    const int workCount = size_.workSide * size_.workSide;      // 0x58cf96
    for (int i = 0; i < workCount; ++i)                         // 0x58cfa7
    {
        WorkCell& work = workCells_[i];
        const int packed = work.MapCoords();                    // 0x58cfb6

        MapCell* cell = CellAt(static_cast<int16_t>(packed & 0xFFFF),
                               static_cast<int16_t>((packed >> 16) & 0xFFFF));

        if (work.data[14] != -1)                                // 0x58cfd0
            continue;
        if ((packed & 0xFFFF) == 0 && ((packed >> 16) & 0xFFFF) == 0)
            continue;                                           // 0x58cfd4

        if (IsWaterFamilyTile(cell) || IsGreenGroundTile(cell)) // 0x58cfe9
            BuildRegionFromCell(cell);                          // 0x58cff4
    }
}

// sub_58BF70 - the region record constructor (0x58bf70 - 0x58c067), shared by
// sub_58C800 (BuildRegionFromCell, which then overwrites level / waterFamily
// with the seed cell's) and by sub_58D620 (SplitOrDropRegion, which keeps them).
//
// Vanilla field writes, in order:
//   0x58bf7a  +0x00 = 0, +0x04 = 0            (no vftable, no neighbour vector)
//   0x58bf8a  +0x08 = dword_ABED14            the next region id
//   0x58bf8d  +0x0C = 0                       cellCount
//   0x58bfa6  +0x10 = anchor cell's Level
//   0x58bfb0  +0x14 = sub_4865D0(anchor)      water family byte
//   0x58bfb9  +0x16 = anchor coords           (not modelled)
//   0x58bfc1  +0x1A = 0                       mergeSettled
//   0x58bfc4  +0x1B = 1                       byte +27 (not modelled)
//   0x58bfc7  +0x1C = 0, +0x20 = 0, +0x24 = 0
//   0x58bfdc  +0x28 = the empty embedded DynamicVectorClass<Cell>
//   0x58bfe3  +0x3C = 10 -> 100000            the vector's growth step
//   0x58bff3  +0x40..+0x4C = 0                the bounding rect
//   0x58c014  ++dword_ABED14
//   0x58c05d  append the object to the global region array
// ---------------------------------------------------------------------------
MapRegion* RandomMapGenerator::CreateRegionRecord(int packedCoords)
{
    MapCell* anchor = CellAt(static_cast<int16_t>(packedCoords & 0xFFFF),
                             static_cast<int16_t>((uint32_t)packedCoords >> 16));

    MapRegion* region = new MapRegion();
    region->id           = regionIdCounter_;                  // 0x58bf8a
    ++regionIdCounter_;                                       // 0x58c014
    region->cellCount    = 0;                                 // 0x58bf8d
    region->level        = anchor->Level;                     // 0x58bfa6
    region->waterFamily  = IsWaterFamilyTile(anchor);         // 0x58bfb0
    region->mergeSettled = 0;                                 // 0x58bfc1
    region->rampFlag     = 1;                                 // 0x58bfc4
    region->consumed     = 0;                                 // 0x58be0e
    region->bounds.X      = 0;                                // 0x58bff3
    region->bounds.Y      = 0;
    region->bounds.Width  = 0;
    region->bounds.Height = 0;

    regions_.push_back(region);                               // 0x58c05d
    return region;
}

// sub_58C800 - "cell -> region record" builder, the shared core of the stage:
// SeedRegionsFromWaterCells and AssignRemainingCells both call it.
//
// Vanilla body (58C800.c, disasm 0x58c816 - 0x58ce7f):
//   1. list = { cell->MapCoords }                    // 0x58c867 (the seed)
//      v48 = sub_4865D0(cell) || sub_4867B0(cell)    // 0x58c882, family flag
//      pBuffer = cell->Level                         // 0x58c89c
//      mark = dword_ABED14                           // the id the record gets
//   2. DFS flood over that list (a stack - it pops from the END). Every popped
//      cell gets work[+56] = mark (the region id) and work[+60] = mark (the
//      visited mark), 0x58c906. A neighbour (Neighbours order, 16-bit adds)
//      joins when it is inside the diamond, its work[+56] is still -1, its
//      work[+60] != mark, its cell Level equals pBuffer and its own family flag
//      equals v48. The visited mark is stamped before those last two tests, so
//      a neighbour failing them is never retried inside this flood. The loop
//      ends when the list drains; `passes` counts the pops.
//   3. passes >= 75 -> keep the region (0x58caa7). Otherwise, when the seed is
//      NOT water / green family, one of two reset paths runs instead:
//        (a) mark != 0 (0x58cac9): look at the west neighbour (seed.x-1,
//            seed.y) - or the north one (seed.x, seed.y-1) when the first is
//            outside the diamond; when both are outside, keep the region
//            (0x58cd34). Else re-stamp every cell carrying `mark` with that
//            neighbour's mark, clear its byte 75, and reset tile / Height /
//            Level to 0 / 0 / the neighbour's Level (0x58cdbf - 0x58cddd),
//            then return 0.
//        (b) mark == 0 (0x58cadc): take the tentative body's boundary ring
//            (sub_5A0700 = BuildWaterRing(0)) and pick one entry at random -
//            ONE RNG draw, scaled by kUnitScale and rejected while out of range
//            (with kUnitScale the scaled value is always < Count, so the
//            rejection never fires; fild at 0x58cb0b reads the 32-bit draw as
//            UNSIGNED). Scan that ring cell's 8 neighbours for an in-diamond one
//            whose cell Level differs from the ring cell's and which is not
//            water family (sub_4865D0 only, 0x58cbc6); on the first hit reset
//            every cell whose work[+56] == 0 back to -1 (byte 75 cleared, tile /
//            Height / Level = 0 / 0 / that neighbour's Level) and return 0. No
//            hit after 8 directions -> keep the region.
//   4. create the record: operator new(0x50) + sub_58BF70(object, cell coords) -
//      the id is dword_ABED14 (0x58bf8a) and the counter is bumped (0x58c014) -
//      then store the seed Level at +16 (0x58ce21), the family flag at byte +20
//      (0x58ce24) and count the cells carrying the id into +12 (0x58ce50 -
//      0x58ce6a; work cells whose coords are (0, 0) - outside the diamond - are
//      skipped via sub_5AC370). sub_58BF70 appends the object to the global
//      region array itself.
//
// MapRegion now also carries the ctor's cell list (+0x28, as MapRegion::cells)
// and the bounding rect (+0x40, as MapRegion::bounds) - the Init-regions stage
// leaves them empty / zeroed, the Making-regions pass sub_58EBC0 fills them; the
// ctor's anchor coords (+22) and byte +27 are still not carried.
//
// Both reset paths write `Level = (saved != -1) ? saved : ::Level`, where the
// global `Level` (@0xABE2E4) is the instance's this[195] = baseLevel_ - see the
// note at the top.
MapRegion* RandomMapGenerator::BuildRegionFromCell(MapCell* cell)
{
    if (cell == nullptr || workCells_ == nullptr)
        return nullptr;

    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    const int side = size_.workSide;

    // ---- 1. Seed the flood list (0x58c816 - 0x58c89c) ---------------------
    const int seedPacked = cell->MapCoords;
    const int16_t seedX = static_cast<int16_t>(seedPacked & 0xFFFF);
    const int16_t seedY = static_cast<int16_t>((uint32_t)seedPacked >> 16);

    std::vector<CellStruct> pending;                 // temp DynamicVectorClass<Cell>
    pending.push_back(CellStruct{ seedX, seedY });   // 0x58c872

    const bool seedFamily = IsWaterFamilyTile(cell)
                         || IsGreenGroundTile(cell); // 0x58c882 (vanilla v48)
    const int  seedLevel  = cell->Level;             // 0x58c89c (vanilla pBuffer)
    const int  mark       = regionIdCounter_;        // dword_ABED14, saved on entry

    // ---- 2. DFS flood (0x58c8aa - 0x58caa7) -------------------------------
    size_t passes = 0;                               // vanilla pMapCoord__1
    while (!pending.empty())
    {
        const CellStruct cur = pending.back();       // 0x58c8b8
        pending.pop_back();                          // 0x58c8c7

        WorkCell& own = workCells_[cur.X + side * cur.Y];
        own.data[14] = mark;                         // 0x58c906 (region id)
        own.data[15] = mark;                         // 0x58c8f4 (visited)

        for (int dir = 0; dir < 8; ++dir)            // 0x58c931
        {
            const int16_t nx = static_cast<int16_t>(cur.X + kDirX[dir]);
            const int16_t ny = static_cast<int16_t>(cur.Y + kDirY[dir]);
            if (!CellExists(nx, ny))                 // 0x58c9bf
                continue;

            WorkCell& nb = workCells_[nx + side * ny];
            if (nb.data[14] != -1)                   // 0x58c9c5
                continue;
            if (nb.data[15] == mark)                 // 0x58c9c5
                continue;
            nb.data[15] = mark;                      // 0x58c9c5, stamped before the tests

            MapCell* neighbour = CellAt(nx, ny);
            if (neighbour->Level != seedLevel)       // 0x58c9e2
                continue;
            const bool nbFamily = IsWaterFamilyTile(neighbour)
                               || IsGreenGroundTile(neighbour);   // 0x58ca13
            if (nbFamily != seedFamily)              // 0x58ca60
                continue;
            pending.push_back(CellStruct{ nx, ny }); // 0x58ca7a
        }
        ++passes;                                    // 0x58ca95
    }

    // ---- 3. Gate / reset paths (0x58caa7 - 0x58cc7f) ----------------------
    bool keep = (passes >= 75) || seedFamily;        // 0x58caa7 / fall-through
    if (!keep)
    {
        if (mark != 0)
        {
            // (a) hand the tentative cells over to a neighbouring region
            //     (0x58cca4 - 0x58cdef).
            int16_t cx = static_cast<int16_t>(seedX - 1);
            int16_t cy = seedY;
            if (!CellExists(cx, cy))                 // 0x58cce6 -> fall back
            {
                cx = seedX;
                cy = static_cast<int16_t>(seedY - 1);
            }

            if (!CellExists(cx, cy))                 // 0x58cd34
            {
                keep = true;                         // -> LABEL_84
            }
            else
            {
                const int handOver = workCells_[cx + side * cy].data[14]; // vanilla v34
                const int handLevel = CellAt(cx, cy)->Level;              // 0x58cd6d

                CellIterator it;                     // 0x58cd79
                it.Reset(cellSlots_, size_.mapWidth, slotStride_);
                while (MapCell* c = it.Next())
                {
                    const int16_t x = static_cast<int16_t>(c->MapCoords & 0xFFFF);
                    const int16_t y = static_cast<int16_t>((uint32_t)c->MapCoords >> 16);
                    WorkCell& w = workCells_[x + side * y];
                    if (w.data[14] != mark)          // 0x58cdbd
                        continue;
                    w.data[14] = handOver;           // 0x58cdbf
                    w.Byte(75) = 0;                  // 0x58cdc2
                    c->IsoTileTypeIndex = 0;         // 0x58cdc6
                    c->Height = 0;                   // 0x58cdc9
                    c->Level = (handLevel != -1) ? handLevel     // 0x58cddd
                                                 : baseLevel_;   // ::Level = this[195]
                }
                return nullptr;                      // LABEL_63 (0x58cc9f)
            }
        }
        else
        {
            // (b) first region of the run: pick a random boundary cell of the
            //     tentative body and look for a neighbour on another Level
            //     (0x58cadc - 0x58cc7f).
            const std::vector<CellStruct> ring = BuildWaterRing(0);   // 0x58cadc
            const int ringCount = static_cast<int>(ring.size());

            int index;
            do
            {
                index = F2I64((double)(uint32_t)rng_.Next()           // 0x58cb03
                              * (double)ringCount * kUnitScale);      // 0x58cb13
            }
            while (index > ringCount - 1);           // 0x58cb20

            const CellStruct pick = ring[static_cast<size_t>(index)]; // 0x58cb2a
            bool handedOver = false;                 // vanilla v16
            for (int dir = 0; dir < 8 && !handedOver; ++dir)   // 0x58cb51
            {
                const int16_t nx = static_cast<int16_t>(pick.X + kDirX[dir]);
                const int16_t ny = static_cast<int16_t>(pick.Y + kDirY[dir]);
                if (!CellExists(nx, ny))             // 0x58cb66
                    continue;

                MapCell* neighbour = CellAt(nx, ny);
                if (neighbour->Level == CellAt(pick.X, pick.Y)->Level) // 0x58cbac
                    continue;
                if (IsWaterFamilyTile(neighbour))    // 0x58cbc6 (sub_4865D0 only)
                    continue;

                const int nbLevel = neighbour->Level;                  // 0x58cbea
                handedOver = true;                   // 0x58cc6e

                CellIterator it;                     // 0x58cbf6
                it.Reset(cellSlots_, size_.mapWidth, slotStride_);
                while (MapCell* c = it.Next())
                {
                    const int16_t x = static_cast<int16_t>(c->MapCoords & 0xFFFF);
                    const int16_t y = static_cast<int16_t>((uint32_t)c->MapCoords >> 16);
                    WorkCell& w = workCells_[x + side * y];
                    if (w.data[14] != 0)             // 0x58cc34
                        continue;
                    w.data[14] = -1;                 // 0x58cc39
                    w.Byte(75) = 0;                  // 0x58cc3c
                    c->IsoTileTypeIndex = 0;         // 0x58cc40
                    c->Height = 0;                   // 0x58cc43
                    c->Level = (nbLevel != -1) ? nbLevel         // 0x58cc56
                                               : baseLevel_;     // ::Level = this[195]
                }
            }

            if (handedOver)
                return nullptr;                      // LABEL_63 (0x58cc85)
            keep = true;                             // LABEL_60 -> LABEL_84
        }
    }

    // ---- 4. Build the region record (0x58cdf6 - 0x58ce7f) -----------------
    // sub_58BF70 constructs it (CreateRegionRecord); sub_58C800 then overwrites
    // the Level (+16) and the family byte (+20) with the seed cell's values.
    MapRegion* region = CreateRegionRecord(seedPacked);  // 0x58ce0e / 0x58bf70
    region->level       = seedLevel;                 // 0x58ce21 (+16)
    region->waterFamily = seedFamily;                // 0x58ce24 (byte +20)

    // Count the cells carrying the region id; work cells that never received
    // diamond coords (data[0] == 0) are skipped (sub_5AC370 against {0, 0}).
    const int workCount = side * side;               // 0x58ce38
    for (int i = 0; i < workCount; ++i)              // 0x58ce71
    {
        if (workCells_[i].data[14] != region->id)    // 0x58ce50
            continue;
        if (workCells_[i].data[0] != 0)              // 0x58ce61
            ++region->cellCount;                     // 0x58ce6a
    }

    // NOTE: deliberately NO append here. CreateRegionRecord (sub_58BF70) already
    // pushed the record onto the array at 0x58c05d - the very address this line
    // used to cite - so pushing it again left TWO pointers to one object in
    // regions_. ReleaseRegionObject erases only the first match, which turned
    // the survivor into a dangling pointer: the next sweep over the array (e.g.
    // RebuildRegionBodies' consumed-record pass) then deleted it a second time
    // and crashed inside ~MapRegion.
    return region;
}

// sub_58E740 - build the body of one region, `passes` (4 / 5 / 6) successive
// fan-out passes starting from the boundary list sub_58D410 returns.
//
// Vanilla body (58E740.c, disasm 0x58e747 - 0x58e97a):
//   list = sub_58D410(region);                    // 0x58e750, region boundary
//   for (pass = 0; pass < a2; ++pass)             // 0x58e760
//   {
//       next = new DynamicVectorClass<Cell>;      // 0x58e768
//       next.growth = 3 * list.count;             // 0x58e7a3 (vector size hint)
//       for each cell of `list`                   // 0x58e7ab - 0x58e941
//         for dir = 0..7 (Neighbours order, 16-bit adds)   // 0x58e7d2
//         {
//           p = cell + Neighbours[dir];           // 0x58e7e5
//           if (!in diamond) continue;            // 0x58e828
//           if (p.Level != region->level) continue;   // 0x58e848 (dword +16,
//                                                     // cell level is a byte)
//           mark = work[p].data[14];              // 0x58e872
//           if (mark == -1 && (sub_486380(p) || sub_4867B0(p)))
//           {                                     // 0x58e88f / 0x58e8a9
//               next.push(p);                     // 0x58e8ef
//               work[p].data[14] = region->id;    // 0x58e903 (dword +8)
//               p.Level = region->level;          // 0x58e90e: BYTE read of
//                                                 // region+0x10 written to the
//                                                 // cell's level byte +0x11B
//           }
//           else if (mark != region->id)
//               return 0;                         // 0x58e91d -> 0x58e986 abort
//         }
//       free list; list = next;                   // 0x58e951 / 0x58e95c
//   }
//   free list; return 1;                          // 0x58e978
//
// The abort is the subtle part: it covers BOTH "the neighbour already belongs
// to another region" and "the neighbour is unassigned (mark == -1) but carries
// a real tile" - a failed sub_486380 / sub_4867B0 test jumps straight to the
// abort comparison (asm 0x58e8b0 -> 0x58e916), it does NOT just skip the
// direction. The caller (InitRegions step 5) ignores the return value, so the
// abort only ends this one region's growth; SweepRegionBody still runs.
//
// RNG: none. The order in which cells are absorbed is the input-list order,
// which sub_58D410 fixes (diamond iterator order), so the pass result is
// deterministic.
//
// MapCell::Level is an int here while vanilla's CellClass level field is a byte
// at +0x11B; every level the generator produces is inside 0..15, so the int
// assignment matches the vanilla byte store.
void RandomMapGenerator::GrowRegionBody(MapRegion* region, int passes)
{
    if (region == nullptr || workCells_ == nullptr)
        return;

    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    const int side = size_.workSide;

    std::vector<CellStruct> current = CollectRegionBoundary(region); // 0x58e750

    for (int pass = 0; pass < passes; ++pass)          // 0x58e760
    {
        std::vector<CellStruct> grown;                 // the pass's new list

        for (size_t i = 0; i < current.size(); ++i)    // 0x58e7ab
        {
            const CellStruct base = current[i];        // 0x58e7d2
            for (int dir = 0; dir < 8; ++dir)          // 0x58e7d2
            {
                const int16_t nx = static_cast<int16_t>(base.X + kDirX[dir]);
                const int16_t ny = static_cast<int16_t>(base.Y + kDirY[dir]);
                if (!CellExists(nx, ny))               // 0x58e828
                    continue;

                MapCell* neighbour = CellAt(nx, ny);
                if (neighbour->Level != region->level) // 0x58e848
                    continue;

                WorkCell& w = workCells_[nx + side * ny];
                const int mark = w.data[14];           // 0x58e872

                if (mark == -1
                    && (IsPlaceholderTile(neighbour)
                        || IsGreenGroundTile(neighbour)))   // 0x58e88f / 0x58e8a9
                {
                    grown.push_back(CellStruct{ nx, ny });  // 0x58e8ef
                    w.data[14] = region->id;                // 0x58e903
                    neighbour->Level = region->level;       // 0x58e90e
                }
                else if (mark != region->id)                // 0x58e91d
                {
                    return;                                 // 0x58e986 abort
                }
            }
        }

        current.swap(grown);                           // 0x58e95c (list = next)
    }
}

// sub_58D410 - boundary-cell collector for one region: the cells carrying the
// region's id that have at least one IN-DIAMOND neighbour with a different
// region mark. GrowRegionBody (pass 0) and SweepRegionBody both start from it.
//
// Vanilla (58D410.c, disasm 0x58d41c - 0x58d592):
//   boundary = new DynamicVectorClass<Cell>;      // 0x58d41c, growth step 10
//   sub_578350();                                 // 0x58d458, iterator reset
//   for (cell = CellIteratorNext(); cell; cell = CellIteratorNext())  // diamond order
//   {
//       if (work[cell].data[14] != region->id) continue;   // 0x58d4a7
//       flag = 0;                                          // 0x58d4b0
//       for (dir = 0; dir < 8; ++dir)                      // 0x58d4b7
//       {
//           n = CellClass::GetNeighbourCell(cell, dir)->MapCoords;
//           if (!in diamond) continue;                     // 0x58d4d7 / 0x58d4e1 /
//                                                          // 0x58d4e9 / 0x58d4f1
//           if (work[n].data[14] != region->id)            // 0x58d519
//               flag = 1;                                  // 0x58d51e
//       }
//       if (flag) boundary.push(cell->MapCoords);          // 0x58d52d / 0x58d570
//   }
//
// Only IN-DIAMOND neighbours count: when a neighbour falls outside the diamond
// the asm jumps straight to the next direction without touching the flag, so a
// region cell that borders nothing but the diamond edge and its own region is
// NOT a boundary cell. The cell is appended once - the whole 8-way scan runs
// first (the asm never breaks out early), and only the flag decides.
//
// RNG: none, and the sweep order is the diamond iterator order, so the list -
// and with it the absorption order inside GrowRegionBody - is deterministic.
std::vector<CellStruct> RandomMapGenerator::CollectRegionBoundary(MapRegion* region) const
{
    std::vector<CellStruct> boundary;
    if (region == nullptr || workCells_ == nullptr || cellSlots_ == nullptr)
        return boundary;

    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    const int side = size_.workSide;

    CellIterator it;                                 // 0x58d458
    it.Reset(cellSlots_, size_.mapWidth, slotStride_);
    while (MapCell* cell = it.Next())                // 0x58d467 / 0x58d57d
    {
        const int16_t x = static_cast<int16_t>(cell->MapCoords & 0xFFFF);
        const int16_t y = static_cast<int16_t>((uint32_t)cell->MapCoords >> 16);
        if (workCells_[x + side * y].data[14] != region->id)   // 0x58d4a7
            continue;

        bool onBoundary = false;                     // vanilla v16
        for (int dir = 0; dir < 8; ++dir)            // 0x58d4b7
        {
            const int16_t nx = static_cast<int16_t>(x + kDirX[dir]);
            const int16_t ny = static_cast<int16_t>(y + kDirY[dir]);
            if (!CellExists(nx, ny))                 // 0x58d4d7 - 0x58d4f1
                continue;
            if (workCells_[nx + side * ny].data[14] != region->id)   // 0x58d519
                onBoundary = true;                   // 0x58d51e
        }

        if (onBoundary)
            boundary.push_back(CellStruct{ x, y });  // 0x58d570
    }

    return boundary;
}

// sub_58E9B0 - second body pass: drain a worklist built from the boundary list
// sub_58D410 returns, absorbing unassigned neighbours one ring at a time.
//
// Vanilla (58E9B0.c, disasm 0x58e9ba - 0x58eb9d):
//   list = sub_58D410(region);                  // 0x58e9ba, the same region
//   worklist = a copy of list                   // 0x58e9c9 - 0x58ea4a
//   free list;
//   while (worklist.count > 0)                  // 0x58ea5c
//   {
//       cell = worklist[count - 1];             // 0x58ea6e, pop the END
//       count--;                                // 0x58ea7a
//       for (dir = 0; dir < 8; ++dir)           // 0x58ea9b (Neighbours order)
//       {
//           n = cell + Neighbours[dir];         // 0x58eab9 - 0x58eac4 (16-bit)
//           if (!in diamond) continue;          // 0x58ead2 - 0x58eafc
//           if (work[n].data[14] != -1) continue;    // 0x58eb15
//           if (work[n].byte75 == 0) continue;       // 0x58eb1b (byte +0x4B)
//           work[n].data[14] = region->id;      // 0x58eb29
//           worklist.push(n);                   // 0x58eb71
//       }
//   }
//   free worklist; return 1;                    // 0x58eb8d
//
// Unlike GrowRegionBody this pass tests NO tile and NO level - only the unused
// region mark plus work byte 75. That flag is the "absorbed by a water stage"
// marker: FindCandidateCenter sets it (0x5a0d6a) and the river / lake rollbacks
// clear it again (0x59e584 / 0x59e5ff / 0x59d404), and the Init-regions
// prologue resets only work[+56] and work[+60], so it survives into this stage.
// The absorbed cell keeps its tile, Height and Level - only the mark changes.
//
// The worklist is a stack: it pops from the end and appends at the end, so a
// freshly absorbed cell is processed before the older entries (depth-first).
// A newly claimed cell can therefore be re-scanned immediately, which is how
// one call fills a whole connected area in a single drain. RNG: none, so the
// order is fully determined by the boundary list sub_58D410 returns.
//
// (The vanilla count-decrement also carries a shift-down loop right after it -
// asm 0x58ea7e `jge` jumps over it unconditionally, because the popped index is
// always count-1 - so the pop really is a plain pop; modelled as pop_back.)
void RandomMapGenerator::SweepRegionBody(MapRegion* region)
{
    if (region == nullptr || workCells_ == nullptr)
        return;

    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    const int side = size_.workSide;

    std::vector<CellStruct> worklist = CollectRegionBoundary(region);  // 0x58e9ba

    while (!worklist.empty())                        // 0x58ea5c
    {
        const CellStruct cur = worklist.back();      // 0x58ea6e
        worklist.pop_back();                         // 0x58ea7a

        for (int dir = 0; dir < 8; ++dir)            // 0x58ea9b
        {
            const int16_t nx = static_cast<int16_t>(cur.X + kDirX[dir]);
            const int16_t ny = static_cast<int16_t>(cur.Y + kDirY[dir]);
            if (!CellExists(nx, ny))                 // 0x58ead2 - 0x58eafc
                continue;

            WorkCell& w = workCells_[nx + side * ny];
            if (w.data[14] != -1)                    // 0x58eb15
                continue;
            if (w.Byte(75) == 0)                     // 0x58eb1b (byte +0x4B)
                continue;

            w.data[14] = region->id;                 // 0x58eb29
            worklist.push_back(CellStruct{ nx, ny });// 0x58eb71
        }
    }
}

// sub_58D010 - assign the cells the seeding pass left unassigned; `flag` is
// written into byte +26 of every region record produced.
//
// Vanilla (58D010.c, disasm 0x58d010 - 0x58d061):
//   count = workSide * workSide;                    // 0x58d016
//   for (i = 0; i < count; ++i, off += 80)          // 0x58d025
//   {
//       if (work[off + 56] != -1) continue;         // 0x58d02c
//       if (!work[off] && !work[off + 2]) continue; // 0x58d032 / 0x58d038
//       cell = MapClass::GetCellAt_MapCrd(work[off]);
//       region = sub_58C800(cell);                  // 0x58d04c
//       if (region) region[26] = flag;              // 0x58d055
//   }
//
// Same sweep shape as SeedRegionsFromWaterCells (linear work order, so the ids
// of these leftover regions follow the same deterministic order), but with two
// differences: there is no water / green-ground gate - every still-unmarked
// diamond cell is handed to the builder, whose own family test decides whether
// it becomes a region or is rolled back - and the builder's return value is
// used, stamping byte +26 with the stage's argument (InitRegions passes 0).
//
// RNG: the only draw any of this can make is BuildRegionFromCell's reset path
// (b), which runs when the cell is neither water family nor green ground and
// the region-id counter is still 0.
void RandomMapGenerator::AssignRemainingCells(unsigned char flag)
{
    if (workCells_ == nullptr)
        return;

    const int workCount = size_.workSide * size_.workSide;   // 0x58d016
    for (int i = 0; i < workCount; ++i)                      // 0x58d025
    {
        WorkCell& work = workCells_[i];
        const int packed = work.MapCoords();                 // work[+0]

        if (work.data[14] != -1)                             // 0x58d02c
            continue;
        if ((packed & 0xFFFF) == 0 && ((packed >> 16) & 0xFFFF) == 0)
            continue;                                        // 0x58d032 / 0x58d038

        MapCell* cell = CellAt(static_cast<int16_t>(packed & 0xFFFF),
                               static_cast<int16_t>((packed >> 16) & 0xFFFF));

        MapRegion* region = BuildRegionFromCell(cell);       // 0x58d04c
        if (region != nullptr)
            region->mergeSettled = flag;                     // 0x58d055 (byte +26)
    }
}
