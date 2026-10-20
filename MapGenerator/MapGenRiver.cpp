// ============================================================================
// MapGenRiver.cpp - river / water-body subsystem of the random map generator
//
// Everything in this file was moved verbatim out of MapGen.cpp; the addresses
// in the comments are the vanilla RMG routines each function replicates.
//
//   GenerateRiver        sub_59D510   river carving + post-processing chain
//   RollbackRiver        LABEL_151    failed-river rollback
//   FindCandidateCenter  sub_5A08D0   candidate search / front growth
//   GenerateDelta        sub_59E740   river-mouth delta fan
//   SmoothWaterBody      sub_57A0C0   four-pass water smoothing
//   ResetPreviewState    sub_4A8BF0   foundation preview reset
//   MarkFoundation       sub_4A95A0   foundation footprint marking
//   TileNeighbourMask    sub_57B210   8-direction connectivity mask
//   FloodFill            sub_57A430   smoothing pass 1
//   CleanupTile          sub_57A320   smoothing pass 2
//   SelectShoreTile      sub_57ACF0   smoothing passes 3 / 4
//   SetFoundationCenter  sub_4A91B0   foundation-preview center move
//   PlaceIsoTile         sub_57B440   isotile foundation stamping
//   BuildWaterRing       sub_5A0700   ring 0 of a water body
//   IsWaterFamilyTile    sub_4865D0   water / shore tile family test
//   ExpandWaterBody      sub_5A0160   ring expansion / absorption
//   DecorateWaterTiles   sub_59C630   whole-map water-detail pass
//   PlaceWaterDetailTile sub_5A6C10   water detail tile stamping
//   LoadTheaterTiles     [General] tile-range snapshots of sub_545150
//
// GenerateLake (sub_59C920) was moved to MapGenLake.cpp.
// ============================================================================

#include "pch.h"
#include "MapGen.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <windows.h>

// ---------------------------------------------------------------------------
// GenerateRiver - sub_59D510 river carving (one attempt). Implemented:
// automatic-mode start + initial angle + width / target length parameters
// (0x59d541 - 0x59d925) + the main-loop skeleton: step (1) center diamond
// check, step (2) cross-section carving with generation-code stamping /
// pollution, step (3) river-mouth delta trigger, step (4) center advance,
// step (5) tributary fork (1% roll each step, gaussian angle in
// [theta+pi/6, theta+5pi/6], recursive call whose return value overwrites
// the alive flag), step (6) angle jitter (steps > 5, gaussian sigma pi/10
// rejection-clamped into theta0 +/- pi/2), step (7) width jitter (gaussian
// sigma 0.5 clamped into the fixed [w0-w0/2, w0+w0/2] window, w0 >= 2
// only), step (8) the 0.5% natural-termination roll
// (0x59d9a3 - 0x59e18d). Post-loop: exit gates + terminal lake (0x59e1a2
// - 0x59e235) and the whole tail (0x59e241 - 0x59e52d): four smoothing
// passes, lakeshore ring expansion, shore tile correction, the mountainous
// canyon branch, the finish expansion and the rollback - implemented. Both of
// the canyon's primitives (FindCandidateCenter / ExpandWaterBody) have landed,
// so the whole tail consumes RNG exactly as vanilla does.
// Full annotated walkthrough: sub_59C580_注释.md §5.2 - §5.9:
//   §5.2 start + initial angle (automatic mode: random edge, gaussian angle)
//   §5.3 river width / target length parameters
//   §5.4 main loop (per-step: carve cross-section, delta trigger, advance,
//        tributary fork, angle/width jitter, natural termination)
//   §5.5 exit conditions + terminal lake
//   §5.6 top-level post-processing (smoothing, outer ring, shore tiles)
//   §5.7 mountainous canyon lift
//   §5.8 success path / §5.9 failure rollback
// ---------------------------------------------------------------------------
bool RandomMapGenerator::GenerateRiver(int* startXY, double initialAngle,
                                       bool isTributary)
{
    int start;     // packed start cell, low16 = X, high16 = Y
    double angle;  // current flow direction theta (radians)

    // Entry snapshot of the tributary flag (vanilla v78, cached 0x59d520
    // before any branch): the 5.6 post-processing gate reads THIS value,
    // not the live parameter - a main river demoted by forking a
    // tributary still gets the full post-processing, only true tributary
    // recursions skip it.
    const bool enteredAsTributary = isTributary;

    if (*startXY != 0)
    {
        // Specified-start branch (tributary recursion, 0x59d7f3): take the
        // given start cell and angle verbatim, no RNG consumed here.
        // (Vanilla also tests a2[2]; the top-level call passes a zeroed
        // start struct and tributary calls always pass an on-map cell, so
        // "*startXY != 0" alone reproduces both vanilla branches.)
        start = *startXY;
        angle = initialAngle;
    }
    else
    {
        // ---- Automatic mode: random edge start (0x59d556 - 0x59d7e2) ----
        const int W = size_.mapWidth;   // W' = MapRect.Width
        const int H = size_.mapHeight;  // H' = MapRect.Height

        // (1) Pick one of the four diamond edges, edge in [0, 3]
        //     (0x59d5c4: F2I64(rand * 4 * kUnitScale), reject > 3)
        int edge = rng_.RandomFloatRange(0, 3);

        // (2) Two free edge parameters, both drawn unconditionally
        //     (0x59d60c / 0x59d659): u in [0, W'-1], v in [0, H'-1]
        int u = rng_.RandomFloatRange(0, W - 1);
        int v = rng_.RandomFloatRange(0, H - 1);

        // (3) Start cell from the 4-entry candidate table (built
        //     0x59d693 - 0x59d762, picked by edge at 0x59d772):
        //       edge=0: (u+1,       W'-u)     edge x+y = W'+1
        //       edge=1: (W'+H'-1-v, H'-v)     edge x-y = W'-1
        //       edge=2: (W'+H'-1-u, H'+u)     line parallel to the
        //                                       x+y = W'+2H' edge, inset 1
        //       edge=3: (v+1,       W'+v)     edge y-x = W'-1
        switch (edge)
        {
        case 0:  start = (u + 1) | ((W - u) << 16); break;
        case 1:  start = (W + H - 1 - v) | ((H - v) << 16); break;
        case 2:  start = (W + H - 1 - u) | ((H + u) << 16); break;
        default: start = (v + 1) | ((W + v) << 16); break;
        }

        // (4) Edge inward base angle (0x59d6a1):
        //       thetaEdge = 7*pi/4 - edge * pi/2
        //     (asm 0x59d67b: fild edge; fmul dbl_7E2820; fsubr dbl_7EDA30.
        //     The pi/2 constant is 0x3FF921FB54442D18, so the literal needs
        //     all 17 digits - the 16-digit 1.570796326794897 parses 2 ulp
        //     high and drifts the sampled edge angle.)
        //     The flow step is (x += cos t, y -= sin t), so each base
        //     angle points from its edge straight into the map.
        double thetaEdge = 5.497787143782138 - edge * 1.5707963267948966;

        // (5) Initial flow angle: gaussian rejection sampling inside
        //     [thetaEdge - pi/4, thetaEdge + pi/4] (0x59d7c5), sigma =
        //     pi/6 (0x59d6a9), center = thetaEdge. The degenerate-range
        //     fallback (0x59d78a) re-centers on the midpoint; it cannot
        //     fire here (pi/6 < pi/4) but is kept verbatim.
        double lo = thetaEdge - 0.7853981633974483;
        double hi = thetaEdge + 0.7853981633974483;
        double sigma = 0.5235987755982988;
        double center = thetaEdge;
        if (center - sigma > hi || center + sigma < lo)
        {
            sigma = (hi - lo) * 0.5;
            center = sigma + lo;
        }
        do
        {
            do
            {
                angle = rng_.Gaussian() * sigma + center;
            } while (angle < lo);
        } while (angle > hi);
    }

    // Start cell must be a valid diamond cell (0x59d817 - 0x59d83e):
    // W' < X+Y <= W'+2H' and |X-Y| < W'; outside -> the attempt fails.
    if (!CellExists(start & 0xFFFF, (uint32_t)start >> 16))
        return false;

    // ---- River width (0x59d84f - 0x59d8b6) ----
    // maxWidth = F2I64(max(waterAmount * 0.07, 1.0)); initial width is
    // drawn from [1, maxWidth] (waterAmount <= 14 pins the width at 1).
    double maxWidthF = waterAmount_ * 0.07;
    if (maxWidthF < 1.0)
        maxWidthF = 1.0;
    int maxWidth = (int)maxWidthF;  // F2I64 truncation
    int width = rng_.RandomFloatRange(1, maxWidth);

    // ---- Target length in [35, 125] cells (0x59d901 - 0x59d925) ----
    // Vanilla multiplies by the pre-folded constant 91 * kUnitScale
    // (2.118758857743526e-08); (rand * 91) * kUnitScale here is identical
    // except for a possible 1-ulp difference that cannot flip F2I64.
    int targetLength = rng_.RandomFloatRange(35, 125);

    // [TEMP DIAG] resolved start and parameters for this attempt.
    DiagLog("RIVER g=%d start=(%d,%d) angle=%.3f width=%d target=%d tributary=%d",
            genCode_, start & 0xFFFF, static_cast<uint32_t>(start) >> 16,
            angle, width, targetLength, static_cast<int>(isTributary));

    // ---- Pre-loop state (0x59d940 - 0x59d9a3) ----
    const double theta0 = angle;  // vanilla `number`: the initial flow angle;
                                  // the step-6 jitter clamps the current
                                  // angle into theta0 +/- pi/2 and the delta
                                  // trigger measures drift against it
    double curWidth = (double)width;   // number_3: current width; lives as a
                                       // double because the width jitter
                                       // (step 7) adds gaussian deltas to it
    double cx = (double)(start & 0xFFFF) + 0.5;          // p_number 0x59d963
    double cy = (double)((uint32_t)start >> 16) + 0.5;   // number_11 0x59d974
    // Step-6 clamp bounds (0x59d988 - 0x59d99c): theta0 +/- pi/2; the pi/2
    // literal 1.5707963267948966 is dbl_7E2820's exact bit pattern.
    const double angleLo = theta0 - 1.5707963267948966;    // v112
    const double angleHi = theta0 + 1.5707963267948966;    // v114
    // Step-7 clamp bounds (0x59d8d7 - 0x59d8ee): [w0 - w0/2, w0 + w0/2]
    // with an integer-division half (cdq / sub / sar 1) - w0 = 5 gives
    // [3, 7], NOT [2.5, 7.5]. widthHalf doubles as the step-7 gate
    // (v86 > 0 <=> w0 >= 2 - a width-1 river never jitters).
    const int    widthHalf = width / 2;                     // v86
    const double widthLo   = curWidth - (double)widthHalf;  // v101
    const double widthHi   = curWidth + (double)widthHalf;  // v113

    bool   alive    = true;  // v73: survival flag; cleared when a carve cell
                             // hits another pass's water (pollution, checked
                             // at the top of the next iteration, 0x59da03)
    int    steps    = 0;     // n0x7D_1: cells advanced
    double termRand = 1.0;   // number_5: natural-termination random
    int    deltaCount = 0;   // v83 (init 0x59d537): river-mouth deltas placed
                             // by this river - the trigger caps it at 1; the
                             // post-loop phases read it too (canyon / rollback
                             // selection, doc 5.7 - 5.9)

    // ---- Main loop (0x59d9a3 - 0x59e18d): all steps (1) - (8) implemented ----
    do
    {
        // (1) Center diamond check (0x59d9a3 - 0x59da03): the center cell
        //     (F2I64(cx), F2I64(cy)) must be a valid diamond cell; the four
        //     asm branches (x+y <= W', |x-y| >= W' twice, x+y > W'+2H') plus
        //     the alive test all jump past the loop.
        if (!CellExists(F2I64(cx), F2I64(cy)) || !alive)
            break;

        // (2) Cross-section carve (0x59da10 - 0x59dc93): one row of cells
        //     perpendicular to the flow. Section direction (sin, cos) is
        //     orthogonal to the flow step (cos, -sin); the row holds
        //     F2I64(width + 0.5) cells centered on the river center, first
        //     cell = center - (count-1)/2 * (sin, cos). Per-cell diamond
        //     gates that fail only skip that cell (0x59dae2 jle / 0x59db0e
        //     jge,jg -> advance) - the section never aborts early.
        //     (std::sin/std::cos vs the game's x87 YRMath: <1 ulp drift,
        //     same accepted deviation class as the Gaussian log/sqrt.)
        double sinT = std::sin(angle);
        double cosT = std::cos(angle);
        int count = F2I64(curWidth + 0.5);

        double px = cx - (double)(count - 1) * sinT * 0.5;  // number_7
        double py = cy - (double)(count - 1) * cosT * 0.5;  // number_8

        // Per-section collinearity flags (v75 / v74[0], set 0x59da4a /
        // 0x59da53): both start set and are cleared the moment the section
        // walk crosses a cell boundary on their axis; still set after the
        // section means every carved cell shared one F2I64 coordinate -
        // the section collapsed onto a single lattice line (a straight
        // column or row of cells).
        bool xCollinear = true;   // v75:  all section cells share X
        bool yCollinear = true;   // v74:  all section cells share Y

        for (int i = count; i > 0; --i)
        {
            int X = F2I64(px);   // 0x59dab9
            int Y = F2I64(py);   // 0x59dac2

            if (CellExists(X, Y))
            {
                WorkCell& wc   = workCells_[X + size_.workSide * Y];
                MapCell*  cell = cellSlots_[512 * Y + X];
                int mark = wc.data[14];   // generation mark, byte 56

                bool carve;
                if (mark == 0)
                {
                    // Free cell: carvable only while the tile is still plain
                    // land - sub_486380 (0x486380): IsoTileTypeIndex is 0 or
                    // 0xFFFF. Any other tile leaves mark 0 != genCode_ and
                    // falls into the pollution branch below.
                    int tile = cell->IsoTileTypeIndex;
                    carve = (tile == 0 || tile == 0xFFFF);
                }
                else
                {
                    carve = (mark == genCode_);   // own pass: re-carve
                }

                if (carve)
                {
                    // 0x59dbad - 0x59dc10: stamp the generation code,
                    // flatten and set the water base tile
                    wc.data[14] = genCode_;
                    cell->Height = 0;
                    cell->IsoTileTypeIndex = waterTileIndex_;
                }
                else
                {
                    // 0x59dc12: the mark belongs to another (older) water
                    // pass -> pollution; the whole attempt fails, but the
                    // rest of this cross-section is still carved
                    DiagLog("RIVER-POLLUTE step=%d cell=(%d,%d) mark=%d",
                            steps, X, Y, mark);
                    alive = false;
                }
            }

            px += sinT;   // 0x59dc1b / 0x59dc42
            py += cosT;
            if (F2I64(px) != X) xCollinear = false;   // 0x59dc25-37: x step
                                                      // crossed a boundary
            if (F2I64(py) != Y) yCollinear = false;   // 0x59dc4b-5f: y step
                                                      // crossed a boundary
        }

        // Post-section collinearity resolve (0x59dc7d - 0x59dca1): the flag
        // on the section's dominant axis is always dropped - |sinT| >=
        // |cosT| clears the x flag (0x59dc9c), else the y flag (0x59dc95).
        // Only a section that collapsed onto one lattice line keeps its
        // minor-axis flag, so at most one of the two can still be set.
        if (std::fabs(sinT) >= std::fabs(cosT))
            xCollinear = false;
        else
            yCollinear = false;

        // (3) River-mouth delta trigger (0x59dca1 - 0x59de18): no RNG. All
        //     six gates must pass - top-level river (tributaries get no
        //     delta), the terminal cross-section collapsed onto one lattice
        //     line, no delta placed yet by this river (at most one), the
        //     flow has drifted less than 1 radian from theta0, the map
        //     rolled "deltas allowed" (random25Flag_ / this[196], the 25%
        //     roll at the end of the land pass), and the walk has outlived
        //     its target length (the river is "looking for the sea").
        if (!isTributary && (xCollinear || yCollinear) && deltaCount < 1)
        {
            // Angle-drift gate (0x59dccb - 0x59dcf6): the drift is truncated
            // to an integer first, then compared against pi/4 - an integer
            // is below 0.785... only when it is 0, so the effective gate is
            // |angle - theta0| < 1 radian.
            if (std::abs(F2I64(angle - theta0)) < 0.7853981633974483
                && random25Flag_
                && steps > targetLength)
            {
                // Fan heading (0x59dd1a - 0x59dd59): quantize the flow
                // direction onto the cardinal axis of the surviving flag -
                //   vertical section (all X equal):   2 = +x (cos > 0),
                //                                     6 = -x (cos <= 0)
                //   horizontal section (all Y equal): 0 = -y (sin > 0),
                //                                     4 = +y (sin <= 0)
                int direction = xCollinear
                    ? (cosT <= 0.0 ? 6 : 2)
                    : (sinT <= 0.0 ? 4 : 0);

                // Delta end points (0x59dd5e - 0x59ddbe): the first and the
                // last cell of the terminal cross-section. The originals are
                // stored as int16 pairs, so pack with a 16-bit mask (an
                // off-map end cell keeps its sign through the high bit).
                bool deltaOk = false;   // v74[0], reused as the output flag
                                       // (pre-cleared 0x59dd66)
                int endX   = F2I64(px - sinT);   // 0x59dd7b: last section cell
                int endY   = F2I64(py - cosT);   // 0x59dd96
                int startX = F2I64(cx - (double)(count - 1) * sinT * 0.5);
                int startY = F2I64(cy - (double)(count - 1) * cosT * 0.5);

                GenerateDelta(genCode_,
                              (startX & 0xFFFF) | ((startY & 0xFFFF) << 16),
                              (endX   & 0xFFFF) | ((endY   & 0xFFFF) << 16),
                              direction, &deltaOk, &cx, &cy);

                if (deltaOk)   // 0x59de00 - 0x59de14
                {
                    ++deltaCount;   // v83
                    ++genCode_;     // fresh code for the continued channel
                }
            }
        }

        // (4) Center advance (0x59de18 - 0x59de3d): one cell-step along the
        //     flow - cx += cos, cy -= sin - reusing the cos/sin already
        //     computed for this step's cross-section (asm loads the same
        //     var_B0 / var_F0 slots, no recompute). All six delta-gate
        //     failure jumps land exactly here, and when a delta fired this
        //     step the advance adds on top of the 12-cell mouth jump
        //     GenerateDelta applied through the cx/cy pointers.
        cx += cosT;
        cy -= sinT;

        // (5) Tributary fork (0x59de44 - 0x59dfa2). The Random() roll is
        //     consumed EVERY step, before the 1% gate (asm 0x59de44: the
        //     call precedes all four tests) - that keeps the stream aligned
        //     even on steps where every gate fails. Gates, in asm order:
        //     roll < 0.01, river still alive, not a tributary, no delta
        //     placed yet (asm 0x59de60 - 0x59de92: zero-extended fild,
        //     fmul dbl_7ED898, fcomp 0.01 - no boundary ambiguity).
        double forkRoll = rng_.Unit();
        if (forkRoll < 0.01 && alive && !isTributary && deltaCount == 0)
        {
            // Demote this river (asm 0x59dea5: a4 = 1): after one fork it
            // can neither fork again nor place a delta, and the pending
            // post-loop phases will treat it as non-top-level.
            isTributary = true;

            // Branch angle window (asm 0x59de98 - 0x59dee4), all constants
            // read from the binary:
            //   center = theta + pi/2    (dbl_7E2820 = 1.5707963267948966)
            //   lo     = theta + pi/6    (dbl_7EDA28 = 0.5235987755982988)
            //   hi     = theta + 5*pi/6  (dbl_7EDA08 = 2.6179938779914944)
            //   sigma  = pi/6            (inline 0x3FE0C152382D7365)
            // The window is centered on theta + pi/2: the tributary leaves
            // the main channel at a right-ish angle (60..120 degrees off
            // the current flow, always the same rotational side).
            double branchCenter = angle + 1.5707963267948966;
            double branchLo     = angle + 0.5235987755982988;
            double branchHi     = angle + 2.6179938779914944;
            double branchSigma  = 0.5235987755982988;

            // Degenerate-window fallback (asm 0x59dee8 - 0x59df34):
            // recenter on the midpoint, sigma = half-width. Unreachable
            // here (pi/6 < pi/3) but kept verbatim.
            if (branchCenter - branchSigma > branchHi
                || branchCenter + branchSigma < branchLo)
            {
                branchSigma = (branchHi - branchLo) * 0.5;
                branchCenter = branchSigma + branchLo;
            }

            // Gaussian rejection sampling (asm 0x59df38 - 0x59df6c) - the
            // same nested do-while shape as the automatic-mode angle.
            double branchAngle;
            do
            {
                do
                {
                    branchAngle = rng_.Gaussian() * branchSigma + branchCenter;
                } while (branchAngle < branchLo);
            } while (branchAngle > branchHi);

            // Tributary start (asm 0x59df6e - 0x59df89): F2I64 of the raw
            // post-walk section coordinates - one full step PAST the last
            // carved cell (the delta trigger instead uses px - sinT, the
            // last carved cell itself). Packed through 16-bit words, so an
            // off-map start keeps its sign bits and fails the callee's
            // diamond check.
            int branchStart = (F2I64(px) & 0xFFFF) | ((F2I64(py) & 0xFFFF) << 16);

            // Recursive carve (asm 0x59df9d); the return value OVERWRITES
            // the alive flag (asm 0x59dfa2: mov [var_121], al): a failed
            // tributary poisons the main river (loop breaks at the next
            // iteration's alive test), a successful one leaves it running.
            // The callee runs the full chain (exit gates 5.5, post-processing
            // 5.6, canyon / finish / rollback 5.7 - 5.9), so a tributary that
            // cannot finish now poisons the trunk exactly like vanilla; one
            // that succeeds leaves the main loop running.
            alive = GenerateRiver(&branchStart, branchAngle, true);
            DiagLog("RIVER-FORK step=%d start=(%d,%d) angle=%.3f result=%d",
                    steps, branchStart & 0xFFFF,
                    static_cast<uint32_t>(branchStart) >> 16,
                    branchAngle, static_cast<int>(alive));
        }

        // (6) Angle jitter (0x59dfab - 0x59e084): once more than 5 cells
        //     have been advanced, every step adds a gaussian delta to the
        //     flow angle. The sampling window is RELATIVE to the current
        //     angle - [angleLo - angle, angleHi - angle] - and the draw
        //     uses sigma = pi/10 centered at 0 while that window fits
        //     inside [-pi/10, pi/10]. The recenter branch (sigma =
        //     half-width, center = window midpoint) is structurally
        //     unreachable: angle starts at theta0, dead center of
        //     [angleLo, angleHi], and the rejection below never lets it
        //     leave, so relHi >= 0 >= relLo always fails the recenter
        //     condition. Kept verbatim anyway, same policy as the
        //     tributary fallback. The pull-back into theta0 +/- pi/2 is
        //     purely the rejection clipping at the window edges.
        if (steps > 5)
        {
            double relHi = angleHi - angle;    // number_7, 0x59dfeb
            double relLo = angleLo - angle;    // number_8, 0x59dffd
            double sigma = 0.3141592653589793; // v106: pi/10 (|dbl_7EDA00|)
            double center = 0.0;               // v108

            if (relHi < -0.3141592653589793 || relLo > 0.3141592653589793)
            {
                sigma = (relHi - relLo) * 0.5;   // 0x59e031
                center = sigma + relLo;          // 0x59e03c
            }

            double jitter;
            do
            {
                do
                {
                    jitter = rng_.Gaussian() * sigma + center;
                } while (jitter < relLo);
            } while (jitter > relHi);
            angle += jitter;   // 0x59e084
        }

        // (7) Width jitter (0x59e08f - 0x59e15c): the same gaussian-window
        //     scheme with sigma = 0.5 around the CURRENT width, clamped by
        //     rejection into the FIXED init-time bounds [w0 - w0/2,
        //     w0 + w0/2] (integer-division half). Gate: widthHalf > 0
        //     (v86, asm 0x59e08b cmp/jle), i.e. only rivers whose initial
        //     width is >= 2 jitter. The recenter branch is unreachable for
        //     the same reason as step 6.
        if (widthHalf > 0)
        {
            double relHi = widthHi - curWidth;   // number_7, 0x59e0c9
            double relLo = widthLo - curWidth;   // number_8, 0x59e0db
            double sigma = 0.5;                  // number_13
            double center = 0.0;                 // v77

            if (relHi < -0.5 || relLo > 0.5)
            {
                sigma = (relHi - relLo) * 0.5;   // 0x59e10f
                center = sigma + relLo;          // 0x59e11a
            }

            double jitter;
            do
            {
                do
                {
                    jitter = rng_.Gaussian() * sigma + center;
                } while (jitter < relLo);
            } while (jitter > relHi);
            curWidth += jitter;   // 0x59e15c: feeds the next section's count
        }

        // (8) Natural termination roll (0x59e16d - 0x59e18d): 0.5% chance per
        //     step. Ships with the loop skeleton because it is the do-while's
        //     own condition update - without it (and before the step 4
        //     advance exists) the loop could never terminate and the UI
        //     would hang on Generate.
        termRand = rng_.Unit();
        ++steps;
    } while (termRand >= 0.005);
    // [SNAPSHOT-OFF] SaveStageSnapshot("termRand");

    // [TEMP DIAG] main loop exited: advanced cells, live flag, last 0.5% roll.
    DiagLog("RIVER-END steps=%d alive=%d term=%.4f",
            steps, static_cast<int>(alive), termRand);

    // ---- 5.5 Exit gates + terminal lake (0x59e1a2 - 0x59e235) ----
    // The loop left either by natural termination (termRand < 0.005, the
    // do-while condition) or by break - the center walked off the diamond
    // or pollution killed the pass. The step-count gate applies to both:
    // fewer than 40 advanced cells is an automatic failure no matter why
    // the loop ended.
    if (steps < 40)
        alive = false;   // 0x59e1ad

    // Terminal lake (0x59e1bf - 0x59e235): natural termination only - a
    // broken-out river keeps its last roll, which was >= 0.005 by
    // construction, so this test distinguishes the two exits. The final
    // center cell must still pass the same four diamond bounds as the
    // loop-top check (asm re-reads the W' / W'+2H' globals - the
    // identical predicate), and the river must still be alive. The lake
    // itself is GenerateLake (sub_59C920), the very routine the top-level
    // lake stage uses, seeded here with the river-mouth cell; a lake
    // failure clears the river's success flag.
    if (termRand < 0.005)
    {
        int endX = F2I64(cx);   // 0x59e1d4
        int endY = F2I64(cy);   // 0x59e1dc
        if (CellExists(endX, endY) && alive)
        {
            int lakeStart = (endX & 0xFFFF) | ((endY & 0xFFFF) << 16);
            if (!GenerateLake(&lakeStart))   // 0x59e22c
                alive = false;               // 0x59e235
        }
    }
    // [SNAPSHOT-OFF] SaveStageSnapshot("lakeStartA");

    // ---- 5.6 Top-level post-processing (0x59e241 - 0x59e307) ----
    // Three-stage pipeline, top-level rivers only (entry-snapshot gate,
    // see enteredAsTributary above), and only while still alive.
    if (!enteredAsTributary && alive)
    {
        // Stage 1 - four full-map smoothing passes (0x59e267): repairs the
        // water cells stamped with the current generation code; failure
        // kills the river.
        alive = SmoothWaterBody(genCode_, 0);
        // [SNAPSHOT-OFF] SaveStageSnapshot("SmoothWaterBody2");

        if (alive)
        {
            // Stage 2 - ring expansion, lakeshore mode (0x59e299): full
            // map - center (0, 0), range (512, 512), no lift. May bump
            // genCode_ (a fresh code for the ring cells). The return
            // value overwrites the success flag (asm 0x59e2a9).
            alive = ExpandWaterBody(genCode_, 1, 0, 0, 512, 512, 0, 0);

            // Stage 3 - shore tile correction (0x59e2ad - 0x59e307):
            // vanilla walks the map's full cell iterator (sub_578350 /
            // MapClass::CellIteratorNext); here the work square, a
            // superset of every cell the pass could have marked. The
            // comparison uses genCode_ RE-READ after the expansion (asm
            // 0x59e29e) - the ring cells carry the bumped code. A marked
            // cell whose tile is still a placeholder - 0 = plain land,
            // 0xFFFF = covered by a multi-cell tile - becomes the
            // green ground tile (game global dword_AA0E18).
            // Runs regardless of the expansion's own result (asm gates
            // this block on the SMOOTHING result only).
            for (int Y = 0; Y < size_.workSide; ++Y)
            {
                for (int X = 0; X < size_.workSide; ++X)
                {
                    if (workCells_[X + size_.workSide * Y].data[14] != genCode_)
                        continue;
                    MapCell* cell = cellSlots_[512 * Y + X];
                    if (cell == nullptr)
                        continue;   // non-diamond cell - never marked
                    int tile = cell->IsoTileTypeIndex;
                    if (tile == 0 || tile == 0xFFFF)
                        cell->IsoTileTypeIndex = greenTileIndex_;  // 0x59e301 (dword_AA0E18)
                }
            }
        }
    }

    // [SNAPSHOT-OFF] SaveStageSnapshot("Top-level");
    // ---- 5.7 canyon branch / 5.8 finish expansion / 5.9 rollback ----
    // (0x59e31c - 0x59e52d, the tail of sub_59D510). The vanilla goto graph is
    // reproduced below with the label names quoted, so each branch can be
    // audited against the original.
    //
    // Local-variable wiring, verified in the decompilation:
    //   v74[0]      canyon flag (the slot also carries the river-mouth Y)
    //   v83         deltaCount
    //   v73         alive
    //   v78 = a4    enteredAsTributary
    //   this[195]   baseLevel_
    //   this[193]   usedWaterCells_
    //   n0x7D_1     steps
    bool canyonLifted = false;                  // v74[0] = 0 (0x59e31c)

    // 0x59e33c: a canyon is only cut by a top-level, still-living river whose
    // base ground level is still the pristine 4, and only when no delta was
    // placed. The lift raises baseLevel_ to 8, so this fires at most once per
    // map (cross-referenced with the "first river wins" rule).
    const bool canyonGate = (deltaCount != 0 || !alive || baseLevel_ != 4);

    if (!canyonGate && !enteredAsTributary)
    {
        // 0x59e358: one main-RNG draw normalised to [0,1) - the asm is
        // Random() * 1/(2^32-1), i.e. R250Random::Unit().
        const double roll = rng_.Unit();
        if (roll < 0.7)                                     // 0x59e37e
        {
            // 0x59e384 - 0x59e3fd. Vanilla builds the search rect inline
            // ({0, 0, 512, 512}, 0x59e3a6 - 0x59e3bb) and passes the river's
            // start cell as the angle anchor (`v88`, written once at
            // 0x59d772 / 0x59d7f3 and never touched again). Both outputs of
            // the search are discarded here.
            const int rect[4] = { 0, 0, 512, 512 };
            const CellStruct anchor{ static_cast<int16_t>(start & 0xFFFF),
                                     static_cast<int16_t>((uint32_t)start >> 16) };
            if (!FindCandidateCenter(genCode_, 0.01, rect, anchor, 1))
                return RollbackRiver(genCode_, deltaCount);  // LABEL_151
            alive = ExpandWaterBody(genCode_, 6, 0, 0, 512, 512, 0, 0);
            // [SNAPSHOT-OFF] SaveStageSnapshot("TExpandWaterBody1");
            if (!alive)
                return RollbackRiver(genCode_, deltaCount);

            // 0x59e40e - 0x59e468: raise every cell that does NOT belong to
            // this river by 4 levels - the carved channel becomes a canyon.
            for (int Y = 0; Y < size_.workSide; ++Y)
            {
                for (int X = 0; X < size_.workSide; ++X)
                {
                    MapCell* cell = cellSlots_[512 * Y + X];
                    if (cell == nullptr)
                        continue;
                    if (workCells_[X + size_.workSide * Y].data[14] != genCode_)
                        cell->Level += 4;
                }
            }
            canyonLifted = true;                            // 0x59e46b
        }
    }

    // LABEL_138 (0x59e482) - both of its entry edges (the blocked gate and the
    // canyon attempt) are top-level only. A tributary reaches LABEL_142/143
    // instead, and one whose gate passed goes straight to LABEL_147.
    if (!enteredAsTributary)
    {
        // LABEL_142 when dead: v64 = (deltaCount <= 0) -> LABEL_143 -> LABEL_147
        // -> LABEL_151, i.e. always a rollback for a dead top-level river.
        if (!alive)
            return RollbackRiver(genCode_, deltaCount);

        // 0x59e484: v64 = (v83 <= 0).
        if (deltaCount == 0)
        {
            // 0x59e490: riverside expansion, skipped when the canyon just ran.
            if (!canyonLifted)
            {
                alive = ExpandWaterBody(genCode_, 2, 0, 0, 512, 512, 0, 0);
                // [SNAPSHOT-OFF] SaveStageSnapshot("ExpandWaterBody2");
                if (!alive)
                    return RollbackRiver(genCode_, deltaCount);  // LABEL_146/147
            }
        }
        else
        {
            // 0x59e4f7: delta present, so the ring is lifted by baseLevel_.
            alive = ExpandWaterBody(genCode_, 2, 0, 0, 512, 512, 1, baseLevel_);
            // [SNAPSHOT-OFF] SaveStageSnapshot("ExpandWaterBody3");
            if (!alive)
                return RollbackRiver(genCode_, deltaCount);
        }
    }
    else if (canyonGate)
    {
        // LABEL_142/143 for a tributary. deltaCount is always 0 here - the
        // delta trigger (0x59dcfc) is trunk-only - so this normally stops at
        // LABEL_147 with no expansion; the branch below keeps the delta case
        // 1:1 anyway.
        if (!alive)
            return RollbackRiver(genCode_, deltaCount);      // LABEL_151
        if (deltaCount > 0)
        {
            alive = ExpandWaterBody(genCode_, 2, 0, 0, 512, 512, 1, baseLevel_);
            // [SNAPSHOT-OFF] SaveStageSnapshot("ExpandWaterBody4");
            if (!alive)
                return RollbackRiver(genCode_, deltaCount);
        }
    }
    // else: tributary with a passing gate -> LABEL_147 directly.

    // LABEL_147 (0x59e508) - budget tail.
    if (!alive)
        return RollbackRiver(genCode_, deltaCount);
    if (canyonLifted)
        baseLevel_ += 4;                                    // 0x59e512
    usedWaterCells_ += steps;                               // 0x59e525
    return true;                                            // 0x59e52d
}

// ---------------------------------------------------------------------------
// RollbackRiver - sub_59D510 LABEL_151 (0x59e538 - 0x59e629)
//
// Puts a failed river back to plain land. Two passes over the map's cell
// iterator, each touching only the cells that carry the code being undone:
//
//   pass 1 (0x59e543): code == genCode
//   pass 2 (0x59e5be): code == genCode - 1, and only when the river placed a
//                      delta (v83 > 0) - that code holds the earlier half of
//                      the channel plus the delta fan
//
// Each hit resets, in the original order (0x59e581 / 0x59e5fc):
//   work data[14]          = 0            generation code
//   work byte 75 (v69[75]) = 0            rollback helper marker
//   cell IsoTileTypeIndex  = 0
//   cell Height            = 0
//   cell Level             = baseLevel_
//
// The generation code itself is NOT decremented: vanilla leaves it bumped, so
// a failed attempt makes the next try skip a code (doc §5.9). Returns false so
// call sites read `return RollbackRiver(...)`.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::RollbackRiver(int genCode, int deltaCount)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1 && deltaCount <= 0)
            break;
        const int code = (pass == 0) ? genCode : genCode - 1;

        for (int Y = 0; Y < size_.workSide; ++Y)
        {
            for (int X = 0; X < size_.workSide; ++X)
            {
                // Vanilla walks the diamond iterator, so non-diamond cells are
                // never visited (and never carry a code in the first place).
                MapCell* cell = cellSlots_[512 * Y + X];
                if (cell == nullptr)
                    continue;
                WorkCell& work = workCells_[X + size_.workSide * Y];
                if (work.data[14] != code)
                    continue;
                work.data[14] = 0;
                work.Byte(75) = 0;
                cell->IsoTileTypeIndex = 0;
                cell->Height           = 0;
                cell->Level            = baseLevel_;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// FindCandidateCenter - sub_5A08D0 (0x5A08D0) - candidate search / front grow.
//
// Called with (ecx = this, edx = genCode, genCode, density, rect, anchor,
// flag). It does more than search: it grows a front of accepted cells and
// stamps them into the work array.
//
//   1. Buffers (0x5a08de): candidate records of 8 bytes {coords, float
//      priority}, capacity max(2*H*W, 100) (never bounds-checked - a cell can
//      enter the heap once), plus a 1-based min-heap of record pointers.
//   2. Base direction from the rect (0x5a0995): X==0 && rangeX!=512 -> pi,
//      Y!=0 -> 3pi/2, Y==0 && rangeY!=512 -> pi/2. A full-map rect lefts it
//      at 0.
//   3. Every diamond cell's work byte 15 is cleared (0x5a0a19).
//   4. Seed sweep over ring 0 (sub_5A0700), keeping the cells inside the rect,
//      scoring each by |angle(cell - anchor) - direction| * 1.5 plus one RNG
//      draw * 2.0 and pushing it (0x5a0a83 - 0x5a0c2e).
//   5. Step budget (0x5a0c40): steps = F2I64(0.5 * max(ln(X),1) / density)
//      with X read from the 4-byte slot whose top byte is the success flag
//      (0x5a0990 writes 1 there) - so X = 0x01xxxxxx, a deliberately huge
//      stand-in that makes the budget density-only. Then one rejection-sampled
//      RNG draw in [0, steps/2] is added (0x5a0c99).
//   6. Growth loop (0x5a0d06): pop the best candidate, mark it as part of the
//      body when it is still free (work byte 75 = 1, data[14] = genCode),
//      scan its FOUR orthogonal neighbours (direction indices 0,2,4,6), and
//      for each free in-rect placeholder neighbour: draw one RNG value, push
//      it with the same angle score, and set work byte 15 = genCode. Any
//      neighbour carrying a different generation clears the success flag.
//      After each pass the reference angle grows by one gaussian * pi/4.
//   7. flag != 0: drain the remaining heap, stamping data[14] = genCode on
//      cells still at 0 and clearing the success flag on foreign codes.
//
// Returns the success flag. RNG draws: one per pushed candidate (seed and
// growth), one per growth step (the gaussian), plus the rejection-sample loop.
// ---------------------------------------------------------------------------

// Direction offsets used by the growth scan - the same Neighbours order as
// GetNeighbourCell / CellClass::GetNeighbourCell (0x89F688).
static const int16_t kCandidateDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int16_t kCandidateDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

bool RandomMapGenerator::FindCandidateCenter(int genCode, double density,
                                             const int* rect,
                                             const CellStruct& anchor, int flag)
{
    // ---- 1. Work buffers (0x5a08de - 0x5a0987) ----------------------------
    // A record is 8 bytes: the packed CellStruct coordinates followed by the
    // float priority at +4. The vanilla capacity is max(2 * H * W, 100) and
    // the buffer is never bounds-checked - it holds because a cell can enter
    // the heap at most once (the work byte-15 marker below).
    struct Candidate
    {
        CellStruct coords;     // +0
        float      priority;   // +4
    };

    int capacity = 2 * size_.width * size_.height;    // 0x5a08f4
    if (capacity <= 100)
        capacity = 100;                               // 0x5a08f8

    std::vector<Candidate> records;
    records.reserve(capacity);          // heap slots point into this

    // Min-heap over candidate pointers. Vanilla's struct is
    // { count, capacity, slots, maxSeen, minSeen } with 1-based slots; only
    // count / slots are ever read back (the max/min fields are dead stores),
    // and a push is silently dropped once count + 1 >= capacity.
    struct MinHeap
    {
        std::vector<Candidate*> slots;   // slots[0] unused
        size_t count = 0;

        void SiftDown(size_t a2)                     // sub_5AD870 (0x5AD870)
        {
            size_t v2 = a2, v3 = 2 * a2;
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
                Candidate* v7 = slots[v2];
                slots[v2] = slots[v3];
                v2 = v3;
                slots[v3] = v7;
                if (2 * v3 <= count && slots[v3]->priority > slots[2 * v3]->priority)
                    v3 *= 2;
                if (v6 <= count && slots[v3]->priority > slots[v6]->priority)
                    v3 = v6;
            }
            while (v3 != v2);
        }

        void Push(Candidate* c)                      // 0x5a0ba7 - 0x5a0c14
        {
            size_t v32 = count + 1;
            size_t v33 = v32 >> 1;
            if (v32 < slots.size())
            {
                while (v32 > 1 && slots[v33]->priority > c->priority)
                {
                    slots[v32] = slots[v33];
                    v32 = v33;
                    v33 >>= 1;
                }
                slots[v32] = c;
                count = v32;                          // ++*v11
            }
        }

        Candidate* Pop()                             // 0x5a0cdc - 0x5a0cf7
        {
            if (count == 0)
                return nullptr;
            Candidate* root = slots[1];
            slots[1] = slots[count];
            slots[count] = nullptr;
            --count;
            SiftDown(1);
            return root;
        }
    } heap;
    heap.slots.assign(capacity + 1, nullptr);

    // ---- 2. Base direction from the rect (0x5a0995 - 0x5a09f3) ------------
    double referenceAngle = 0.0;                     // v10
    if (rect[0] == 0 && rect[2] != 512)
        referenceAngle = 3.141592653589793;
    if (rect[1] != 0)
        referenceAngle = 4.71238898038469;
    else if (rect[3] != 512)
        referenceAngle = 1.5707963267948966;

    const bool success = true;                       // pFoundationData_3 = 1

    // ---- 3. Clear the visited marker (0x5a0a19) --------------------------
    for (int Y = 0; Y < size_.workSide; ++Y)
    {
        for (int X = 0; X < size_.workSide; ++X)
        {
            if (cellSlots_[512 * Y + X] == nullptr)
                continue;                            // iterator walks cells
            workCells_[X + size_.workSide * Y].Byte(15) = 0;
        }
    }

    // ---- 4. Seed sweep over the body's ring 0 (0x5a0a58 - 0x5a0c2e) ------
    const std::vector<CellStruct> ring = BuildWaterRing(genCode);
    size_t recordCount = 0;                          // v86 / n8_7 counter

    for (size_t k = 0; k < ring.size(); ++k)
    {
        const CellStruct c = ring[k];
        const int x = c.X;
        const int y = c.Y;

        if (x < rect[0] || x >= rect[0] + rect[2])
            continue;                                // 0x5a0a94 - 0x5a0aa3
        if (y < rect[1] || y >= rect[1] + rect[3])
            continue;                                // 0x5a0aaf - 0x5a0abe

        // Angle from the anchor, measured against the base direction.
        const int dx = x - anchor.X;                  // n8_2
        const int dy = y - anchor.Y;                  // v26
        double angle;
        if (dx != 0)
        {
            angle = TableAtan(-((double)dy / (double)dx));   // sub_4CADE0 (table)
            if (dx < 0)
                angle += 3.141592653589793;           // 0x5a0b06
        }
        else
        {
            angle = 1.570796326794897;                // 0x5a0be9
        }

        double delta = std::fabs(angle - referenceAngle);
        while (delta >= 6.283185307179586)            // 0x5a0b23
            delta -= 6.283185307179586;
        if (delta > 3.141592653589793)                // 0x5a0b42
            delta = 6.283185307179586 - delta;

        if (records.size() >= (size_t)capacity)
            break;                                    // vanilla would overrun
        records.push_back(Candidate{ c, 0.0f });
        Candidate* record = &records.back();
        record->priority = (float)(delta * 1.5 +
            (double)rng_.Next() * kUnitScale * 2.0);     // 0x5a0b9f
        ++recordCount;                                // 0x5a0b91
        heap.Push(record);                            // 0x5a0ba7
    }

    // ---- 5. Step budget (0x5a0c40 - 0x5a0cc8) ----------------------------
    // The log argument is the 4-byte slot that also holds the success flag in
    // its top byte (0x5a0990 writes 1 there), so the value is 0x01xxxxxx -
    // deliberately huge, making the budget a function of the density alone.
    double logArg = (double)0x01000000;               // ln = 16.6355323...
    double v37 = std::log(logArg);
    if (v37 < 1.0)
        v37 = 1.0;
    int steps = F2I64(1.0 / (1.0 / v37 * density) * 0.5);   // n8_7
    const int halfSteps = steps / 2;                        // cdq/sub/sar

    int offset;                                       // v38
    do
    {
        offset = F2I64((double)(uint32_t)rng_.Next() *
                       (double)(halfSteps + 1) * (1.0 / 4294967295.0));
    }
    while (offset > halfSteps);                       // 0x5a0cc0
    steps += offset;

    Candidate* current = heap.Pop();                  // 0x5a0cdc - 0x5a0cf7

    // ---- 6. Growth loop (0x5a0d06 - 0x5a1045) ----------------------------
    // Each pass takes the best candidate, marks it if it is still free, scans
    // its four ORTHOGONAL neighbours (direction index steps 0,2,4,6 - the
    // scan only uses 4*(v44 & 7) with v44 = n8 + 2), pushes the accepted ones
    // and jitters the reference angle by one gaussian * pi/4.
    bool ok = success;                                // pFoundationData_3
    for (int taken = 0; taken < steps && ok && current != nullptr; ++taken)
    {
        const int cx = current->coords.X;
        const int cy = current->coords.Y;

        WorkCell& own = workCells_[cx + size_.workSide * cy];
        if (own.data[14] == 0)                        // 0x5a0d4a
        {
            MapCell* ownCell = cellSlots_[512 * cy + cx];
            if (IsPlaceholderTile(ownCell))
            {
                own.Byte(75) = 1;                     // 0x5a0d6a
                own.data[14] = genCode;               // 0x5a0d6e
            }
        }

        for (int dirStep = 0; dirStep < 8; dirStep += 2)
        {
            const int dir = dirStep & 7;
            const int16_t nx = static_cast<int16_t>(cx + kCandidateDirX[dir]);
            const int16_t ny = static_cast<int16_t>(cy + kCandidateDirY[dir]);

            if (!CellExists(nx, ny))                  // 0x5a0de6
                continue;

            WorkCell& nw = workCells_[nx + size_.workSide * ny];
            const int  nCode = nw.data[14];
            const bool inRect = (nx >= rect[0] && nx < rect[0] + rect[2] &&
                                 ny >= rect[1] && ny < rect[1] + rect[3]);
            MapCell*   nCell = cellSlots_[512 * ny + nx];

            if (nCode == 0 && nw.Byte(15) != genCode && inRect)
            {
                if (IsPlaceholderTile(nCell))         // 0x5a0e69
                {
                    const int dx = nx - anchor.X;
                    const int dy = ny - anchor.Y;
                    double angle;
                    if (dx != 0)
                    {
                        angle = TableAtan(-((double)dy / (double)dx));   // sub_4CADE0 (table)
                        if (dx < 0)
                            angle += 3.141592653589793;   // 0x5a0ec2
                    }
                    else
                    {
                        angle = 1.570796326794897;        // 0x5a0fe5
                    }

                    double delta = std::fabs(angle - referenceAngle);
                    while (delta >= 6.283185307179586)    // 0x5a0edf
                        delta -= 6.283185307179586;
                    if (delta > 3.141592653589793)        // 0x5a0f01
                        delta = 6.283185307179586 - delta;

                    if (records.size() < (size_t)capacity)
                    {
                        records.push_back(Candidate{ CellStruct{ nx, ny }, 0.0f });
                        Candidate* record = &records.back();
                        record->priority = (float)(delta * 1.5 +
                            (double)rng_.Next() * kUnitScale * 2.0);
                        nw.Byte(15) = static_cast<unsigned char>(genCode);  // 0x5a0f6a
                        ++recordCount;
                        heap.Push(record);            // 0x5a0f81
                    }
                    continue;                          // LABEL_108
                }
            }

            if (nCode != genCode)                      // 0x5a1023
                ok = false;                            // foreign water
        }

        // One gaussian * pi/4 per step keeps the fan from locking up.
        referenceAngle += rng_.Gaussian() * 0.7853981633974483;   // 0x5a104c
        current = heap.Pop();                          // 0x5a10xx
    }

    // ---- 7. Leftover-heap sweep (0x5a10aa - 0x5a115b) --------------------
    if (flag != 0)
    {
        for (Candidate* leaf = heap.Pop(); leaf != nullptr && ok; leaf = heap.Pop())
        {
            WorkCell& w = workCells_[leaf->coords.X +
                                     size_.workSide * leaf->coords.Y];
            if (w.data[14] == 0)
                w.data[14] = genCode;                  // 0x5a111d
            else if (w.data[14] != genCode)
                ok = false;                            // 0x5a1128
        }
    }

    return ok;                                         // 0x5a117d
}

// ---------------------------------------------------------------------------
// GenerateDelta - sub_59E740 river-mouth delta fan - IMPLEMENTED.
//
// Called from the river main loop's delta trigger (0x59ddf5) after all six
// gates pass. Carves a fan of water downstream of the river end; on success
// it also advances the river mouth center 12 cells along the fan heading
// (success exit 0x5a0008 - 0x5a0033: X += [0,12,0,-12][dir/2],
// Y += [-12,0,12,0][dir/2]) so the main channel continues past the fan.
//   genCode     generation code stamped on the fan cells (this[194])
//   startXY     packed first cell of the terminal cross-section (v94)
//   endXY       packed last cell of the terminal cross-section (v104)
//   direction   fan heading: 0 = -y, 2 = +x, 4 = +y, 6 = -x
//   success     output flag (vanilla v74[0]: cleared at entry 0x59e754,
//               written at exit 0x5a003f; also the return value)
//   centerX/Y   river mouth center (vanilla &p_number / &number_11),
//               advanced by the fan on success
// ---------------------------------------------------------------------------
void RandomMapGenerator::GenerateDelta(int genCode, int startXY, int endXY,
                                       int direction, bool* success,
                                       double* centerX, double* centerY)
{
    const int startX = static_cast<int16_t>(startXY & 0xFFFF);
    const int startY = static_cast<int16_t>((uint32_t)startXY >> 16);
    const int endX   = static_cast<int16_t>(endXY & 0xFFFF);
    const int endY   = static_cast<int16_t>((uint32_t)endXY >> 16);

    *success = false;                                  // 0x59e754

    // ---- A. Per-direction frame (0x59e7a7 - 0x59e9f4) --------------------
    // Four switch arms fill the same set of locals with direction-specific
    // band origins/spans. Vanilla's naming, kept for audit:
    //   scan*  <- X_8 / n12 / n12_1 / pMapCoord__20   (occupancy screen)
    //   band*  <- X_9 / n4 / n4_1 / pMapCoord__21     (first fan fill)
    //   wave*  <- MapCoords_9 / n8 / n8_1 / pMapCoord__22 (second fill)
    //   fan*   <- X_5 / Y / n512 / n512_1             (search rect + expansion)
    //   anchor <- v123                                (search anchor)
    int scanX = 0, scanY = 0, scanSpanX = 0, scanCountY = 0;
    int bandX = 0, bandY = 0, bandSpanX = 0, bandCountY = 0;
    int waveX = 0, waveY = 0, waveSpanX = 0, waveCountY = 0;
    int fanX = 0, fanY = 0, fanW = 0, fanH = 0;
    int anchorX = 0, anchorY = 0;
    int family = -1;                                   // nIdx_0..3 by heading

    switch (direction)
    {
    case 0:                                            // heading -y (0x59e7bc)
    {
        const int span = endX - startX;                // v13
        scanY = startY - 12; scanCountY = 12;  scanX = startX - 2; scanSpanX = span + 5;
        bandY = startY - 4;  bandCountY = 4;   bandX = startX;     bandSpanX = span + 1;
        waveY = startY - 12; waveCountY = 8;   waveX = startX;     waveSpanX = span + 1;
        fanX = 0; fanY = startY - 4; fanW = 512; fanH = 512 - (startY - 4);
        anchorX = startX; anchorY = startY - 4;
        family = waterFamily4Base_[3];                 // WaterfallNorth
        break;
    }
    case 2:                                            // heading +x (0x59e8e1)
    {
        const int span = endY - startY;                // v21
        scanY = startY - 2; scanCountY = span + 5; scanX = startX + 1; scanSpanX = 12;
        bandY = startY;     bandCountY = span + 1; bandX = startX + 1; bandSpanX = 4;
        waveY = startY;     waveCountY = 8;        waveX = startX + 5; waveSpanX = span + 1;
        fanX = 0; fanY = 0; fanW = startX + 4; fanH = 512;
        anchorX = startX + 5; anchorY = startY;
        family = waterFamily4Base_[0];                 // WaterfallEast
        break;
    }
    case 4:                                            // heading +y (0x59e855)
    {
        const int span = startX - endX;                // v18
        scanY = endY + 1; scanCountY = 12;     scanX = endX - 2; scanSpanX = span + 5;
        bandY = endY + 1; bandCountY = 4;      bandX = endX;     bandSpanX = span + 1;
        waveY = endY + 5; waveCountY = 8;      waveX = endX;     waveSpanX = span + 1;
        fanX = 0; fanY = 0; fanW = 512; fanH = endY + 4;
        anchorX = endX; anchorY = endY + 1;
        family = waterFamily4Base_[2];                 // WaterfallSouth
        break;
    }
    case 6:                                            // heading -x (0x59e975)
    {
        const int span = startY - endY;                // v24
        scanY = endY - 2; scanCountY = span + 5; scanX = endX - 12; scanSpanX = 12;
        bandY = endY;     bandCountY = span + 1; bandX = endX - 4;  bandSpanX = 4;
        waveY = endY;     waveCountY = span + 1; waveX = endX - 12; waveSpanX = 8;
        fanX = endX - 4;  fanY = 0; fanW = 512 - (endX - 4); fanH = 512;
        anchorX = endX - 4; anchorY = endY;
        family = waterFamily4Base_[1];                 // WaterfallWest
        break;
    }
    default:
        return;                                        // vanilla: empty arm
    }

    // ---- B. Occupancy screen (0x59e9fa - 0x59eadc) -----------------------
    // Any occupied or non-placeholder cell inside the scan band aborts the
    // fan. Vanilla's early exits return 1 but leave *success at 0, so the
    // caller sees "no delta".
    for (int y = scanY; y < scanY + scanCountY; ++y)
    {
        for (int x = scanX; x < scanX + scanSpanX; ++x)
        {
            if (!CellExists(x, y))
                continue;                              // 0x59ea4e
            if (workCells_[x + size_.workSide * y].data[14] != 0)
                return;                                // 0x59ea6b
            if (!IsPlaceholderTile(cellSlots_[512 * y + x]))
                return;                                // 0x59eaca
        }
    }

    // ---- C. First fan fill (0x59eadc - 0x59ec43) -------------------------
    // Each cell gets a random water variant (6-way) and a random Height
    // (4-way), both by rejection sampling, then the generation code.
    for (int y = bandY; y < bandY + bandCountY; ++y)
    {
        for (int x = bandX; x < bandX + bandSpanX; ++x)
        {
            if (!CellExists(x, y))
                continue;                              // 0x59eb42
            int variant;
            do
            {
                variant = F2I64((double)(uint32_t)rng_.Next() * 6.0 * kUnitScale);
            }
            while (variant > 5);                       // 0x59eb77
            MapCell* cell = cellSlots_[512 * y + x];
            cell->IsoTileTypeIndex = waterTileIndex_ + variant;   // 0x59eba0
            int height;
            do
            {
                height = F2I64((double)(uint32_t)rng_.Next() * 4.0 * kUnitScale);
            }
            while (height > 3);                        // 0x59ebd6
            cell->Height = height;                     // 0x59ebf5
            workCells_[x + size_.workSide * y].data[14] = genCode; // 0x59ec1e
        }
    }

    // ---- D. Search / smoothing / expansion (0x59ec6a - 0x59ed60) ---------
    const CellStruct anchor{ static_cast<int16_t>(anchorX),
                             static_cast<int16_t>(anchorY) };
    const int rect[4] = { fanX, fanY, fanW, fanH };
    bool ok = FindCandidateCenter(genCode, 0.003, rect, anchor, 0);  // 0x59ec6a
    if (ok)
    {
        ok = SmoothWaterBody(genCode_, 0);          

        // [SNAPSHOT-OFF] SaveStageSnapshot("SmoothWaterBody");// 0x59ec8d
        if (ok)
        {
            ok = ExpandWaterBody(genCode_, 2, fanX, fanY, fanW, fanH, 0, 0); // 0x59ecd5
            // [SNAPSHOT-OFF] SaveStageSnapshot("ExpandWaterBody0");
            if (ok)
            {
                // 0x59ecee - 0x59ed36: raise every cell of THIS fan.
                for (int Y = 0; Y < size_.workSide; ++Y)
                {
                    for (int X = 0; X < size_.workSide; ++X)
                    {
                        MapCell* cell = cellSlots_[512 * Y + X];
                        if (cell == nullptr)
                            continue;
                        if (workCells_[X + size_.workSide * Y].data[14] == genCode)
                            cell->Level += 4;
                    }
                }

                // 0x59ed53 - 0x59eec4: the second band, same recipe.
                for (int y = waveY; y < waveY + waveCountY; ++y)
                {
                    for (int x = waveX; x < waveX + waveSpanX; ++x)
                    {
                        if (!CellExists(x, y))
                            continue;
                        int variant;
                        do
                        {
                            variant = F2I64((double)(uint32_t)rng_.Next() *
                                            6.0 * kUnitScale);
                        }
                        while (variant > 5);
                        MapCell* cell = cellSlots_[512 * y + x];
                        cell->IsoTileTypeIndex = waterTileIndex_ + variant;
                        int height;
                        do
                        {
                            height = F2I64((double)(uint32_t)rng_.Next() *
                                           4.0 * kUnitScale);
                        }
                        while (height > 3);
                        cell->Height = height;
                        workCells_[x + size_.workSide * y].data[14] = genCode;
                    }
                }

                // ---- E. Waterfall dressing (0x59eee1 - 0x59ffd4) ----------
                // Four direction arms, each: one family start piece plus one
                // end piece via SetFoundationCenter + PlaceIsoTile, a run of
                // 0xFFFF / Level adjustments, two diamond-gated corner blocks
                // and an alternating stamp run. The stamps go through
                // PlaceIsoTile, which currently carries geometry for the
                // shore family only, so the waterfall pieces are placed as
                // no-ops (see PlaceIsoTile).
                auto stamp = [&](int x, int y, int tileIndex, int level)
                {
                    currentBuildingType_ = tileIndex;
                    SetFoundationCenter(CellStruct{ static_cast<int16_t>(x),
                                                    static_cast<int16_t>(y) });
                    bool placed = true;
                    PlaceIsoTile(0, 0x10000, level, genCode, &placed, 0);
                };
                auto markCorner = [&](int x, int y)
                {
                    if (!CellExists(x, y))
                        return;
                    MapCell* cell = cellSlots_[512 * y + x];
                    cell->IsoTileTypeIndex = 0xFFFF;
                    cell->Level += 4;
                    cell->Height = 0;
                    workCells_[x + size_.workSide * y].data[14] = genCode;
                };

                int baseX, baseY, level;
                switch (direction)
                {
                case 0:
                    baseX = bandX - 2;  baseY = bandY - 2;               // 0x59fb4d
                    level = cellSlots_[512 * (bandY - 4) + (bandX - 2)]->Level;
                    stamp(baseX, baseY, family, level);                  // 0x59fbd6
                    stamp(baseX + waveSpanX + 2, baseY, family + 3, level); // 0x59fc55
                    cellSlots_[512 * baseY + (baseX + 1)]->IsoTileTypeIndex = 0xFFFF;
                    cellSlots_[512 * baseY + (baseX + waveSpanX + 2)]->IsoTileTypeIndex = 0xFFFF;
                    for (int k = 0; k < waveSpanX + 2; ++k)              // 0x59fcdb
                        cellSlots_[512 * baseY + (baseX + k + 1)]->Level -= 4;
                    markCorner(baseX - 1, baseY + 1);                    // 0x59fdba
                    markCorner(baseX + waveSpanX + 4, baseY + 1);        // 0x59fe6b
                    for (int k = 0; k < waveSpanX; )                     // 0x59fef9
                    {
                        if ((waveSpanX - k) % 2) { stamp(baseX + k + 2, baseY + 1, family + 1, level); k += 1; }
                        else                     { stamp(baseX + k + 2, baseY + 1, family + 2, level); k += 2; }
                    }
                    break;
                case 2:
                    baseX = waveX;      baseY = waveY + waveCountY;      // 0x59eef4
                    level = cellSlots_[512 * (baseY + 2) + (baseX + 2)]->Level;
                    stamp(baseX, baseY, family, level);                  // 0x59ef7c
                    stamp(baseX, baseY - (waveCountY + 2), family + 3, level); // 0x59eff7
                    cellSlots_[512 * (baseY - (waveCountY + 3)) + baseX]->IsoTileTypeIndex = 0xFFFF;
                    cellSlots_[512 * (baseY + 2) + baseX]->IsoTileTypeIndex = 0xFFFF;
                    for (int k = 0; k < waveCountY + 2; ++k)             // 0x59f467
                        cellSlots_[512 * (baseY - k) + baseX]->Level -= 4;
                    markCorner(baseX, baseY + 2);                        // 0x59f53e
                    markCorner(baseX + 1, baseY - (waveCountY + 3));     // 0x59f5fb
                    for (int k = 0; k < waveCountY; )                    // 0x59f1d8
                    {
                        if ((waveCountY - k) % 2) { stamp(baseX, baseY - (k + 1), family + 1, level); k += 1; }
                        else                     { stamp(baseX, baseY - (k + 2), family + 2, level); k += 2; }
                    }
                    break;
                case 4:
                    baseX = bandX - 2;  baseY = waveY;                   // 0x59f784
                    level = cellSlots_[512 * (baseY + 2) + (baseX + 2)]->Level;
                    stamp(baseX, baseY, family, level);                  // 0x59f80c
                    stamp(baseX + waveSpanX + 2, baseY, family + 3, level); // 0x59f88b
                    cellSlots_[512 * baseY + (baseX - 1)]->IsoTileTypeIndex = 0xFFFF;
                    cellSlots_[512 * baseY + (baseX + waveSpanX + 4)]->IsoTileTypeIndex = 0xFFFF;
                    for (int k = 0; k < waveSpanX + 2; ++k)              // 0x59fa30
                        cellSlots_[512 * baseY + (baseX + k + 1)]->Level -= 4;
                    markCorner(baseX - 1, baseY);                        // 0x59f912
                    markCorner(baseX + waveSpanX + 4, baseY);            // 0x59f9c7
                    for (int k = 0; k < waveSpanX; )                     // 0x59fa4b
                    {
                        if ((waveSpanX - k) % 2) { stamp(baseX + k + 2, baseY, family + 1, level); k += 1; }
                        else                     { stamp(baseX + k + 2, baseY, family + 2, level); k += 2; }
                    }
                    break;
                default: // 6
                    baseX = waveX - 2;  baseY = waveY + waveCountY;      // 0x59f2d5
                    level = cellSlots_[512 * (baseY + 2) + (baseX - 2)]->Level;
                    stamp(baseX, baseY, family, level);                  // 0x59f362
                    stamp(baseX, baseY - (waveCountY + 2), family + 3, level); // 0x59f3dd
                    cellSlots_[512 * (baseY + 2) + baseX]->IsoTileTypeIndex = 0xFFFF;
                    cellSlots_[512 * (baseY - (waveCountY + 2)) + baseX]->IsoTileTypeIndex = 0xFFFF;
                    for (int k = 0; k < waveCountY + 2; ++k)             // 0x59f467
                        cellSlots_[512 * (baseY - k) + baseX]->Level -= 4;
                    markCorner(baseX + 1, baseY + 2);                    // 0x59f53e
                    markCorner(baseX + 1, baseY - (waveCountY + 3));     // 0x59f5fb
                    for (int k = 0; k < waveCountY; )                    // 0x59f690
                    {
                        if ((waveCountY - k) % 2) { stamp(baseX + 1, baseY - (k + 1), family + 1, level); k += 1; }
                        else                     { stamp(baseX + 1, baseY - (k + 2), family + 2, level); k += 2; }
                    }
                    break;
                }
            }
        }
        ok = ok;                                       // v40 = v122 (0x59ffd4)
    }

    // ---- F. River-mouth advance (0x59ffe4 - 0x5a003f) --------------------
    if (ok)
    {
        static const int kMouthX[4] = { 0, 12, 0, -12 };   // X_5/Y/n512/n512_1
        static const int kMouthY[4] = { -12, 0, 12, 0 };   // X_9/pMapCoord__21/n4_1/n4
        *centerX += kMouthX[direction / 2];
        *centerY += kMouthY[direction / 2];
    }
    *success = ok;                                     // 0x5a003f
}

// ---------------------------------------------------------------------------
// SmoothWaterBody - sub_57A0C0 four full-map smoothing passes.
//
// Repairs the water cells stamped with the given generation code (edge
// cleanup after carving). Call sites: river post-processing (0x59e267),
// lake post-processing (0x59d289) and delta post-processing (0x59ec8d) -
// always (genCode, 0). Failure clears the caller's success flag.
//
// Structure (0x57a0c0 - 0x57a309):
//   0. ResetPreviewState()          (sub_4A8BF0, DisplayClass foundation reset)
//   1. Lazy-allocate the work array if absent. Vanilla allocates the global
//      dword_ABED10 only when it is null and frees it again at exit when it
//      did. Here workCells_ is a class member created in GenerateMapBody, so
//      this is normally a no-op and the matching free is unnecessary.
//   2. Reset EVERY work cell's processing marker data[16] to -1 (a flat
//      linear sweep over workSide^2 cells, not the diamond iterator).
//   3. Four passes over the diamond cell iterator sharing ONE success flag
//      (vanilla `j`, initialised to 1):
//        pass 1: FloodFill(cell, genCode, flag)          sub_57A430
//        pass 2: CleanupTile(cell, genCode)              sub_57A320
//                (vanilla ignores this return value)
//        pass 3: SelectShoreTile(cell, 1, genCode, flag) sub_57ACF0
//        pass 4: SelectShoreTile(cell, 2, genCode, flag) sub_57ACF0
//      Each pass is an independent iterator loop whose body begins with
//      "if (!ok) break;" - so a failure stops that pass, but the following
//      passes still run and break out immediately. The returned value is the
//      LAST pass's result, exactly like vanilla returning the shared `j`.
//
// RNG note: SelectShoreTile consumes exactly one RandomFloatRange(0, 5) per
// visited cell, so passes 3 + 4 together consume 2 * (diamond cell count)
// draws in iterator order. FloodFill / CleanupTile consume none.
//
// Dropped from the vanilla body (no effect on map data): the
// MouseClass::CurrentBuilding / CurrentBuildingType teardown at 0x57a2ca.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::SmoothWaterBody(int genCode, int flag)
{
    DiagLog("SMOOTH g=%d flag=%d", genCode, flag);
    ResetPreviewState();

    if (!workCells_)
        InitWorkArray();

    // Reset every work cell's processing marker (data[16]) to -1.
    const int workCount = size_.workSide * size_.workSide;
    for (int i = 0; i < workCount; ++i)
        WorkAtLinear(i).data[16] = -1;

    // One success flag shared by all four passes.
    bool ok = true;

    // Pass 1: recursive flood fill of the water body.
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth);
        while (MapCell* cell = it.Next())
        {
            if (!ok)
                break;
            ok = FloodFill(cell, genCode, flag);
        }
    }

    // [SNAPSHOT-OFF] SaveStageSnapshot("FloodFill");
    // Pass 2: placeholder tile cleanup (return value deliberately ignored).
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth);
        while (MapCell* cell = it.Next())
        {
            if (!ok)
                break;
            CleanupTile(cell, genCode);
        }
        // [SNAPSHOT-OFF] SaveStageSnapshot("CleanupTile");
    }

    // [FIX] Carve staircase-bend inner tips before shore pieces are chosen,
    // so the bend resolves to one continuous piece instead of piece22/piece14
    // fighting at an internal seam.
    if (ok)
        FillStaircaseBends(genCode);
    // [SNAPSHOT-OFF] SaveStageSnapshot("FillStaircaseBends");

    // [FIX] 铺岸片前把"2x2 岸片无解"的对角水角填回陆地（每处 1 格）。
    if (ok)
        FillUnsolvableShoreCorners(genCode);

    // Pass 3: shore tile selection, first round.
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth);
        while (MapCell* cell = it.Next())
        {
            if (!ok)
                break;
            ok = SelectShoreTile(cell, 1, genCode, flag);
        }
        // [SNAPSHOT-OFF] SaveStageSnapshot("SelectShoreTile");
    }

    // Pass 4: shore tile selection, second round - its result is the return.
    {
        CellIterator it;
        it.Reset(cellSlots_, size_.mapWidth);
        while (MapCell* cell = it.Next())
        {
            if (!ok)
                break;
            ok = SelectShoreTile(cell, 2, genCode, flag);
        }
        // [SNAPSHOT-OFF] SaveStageSnapshot("SelectShoreTile2");
    }

    return ok;
}

// ---------------------------------------------------------------------------
// FillStaircaseBends - staircase river-bend repair (transplant-side fix).
//
// A narrow river that turns a sharp corner can leave two diagonally adjacent
// land cells A=(x,y) and B: water occupies the whole row north of A, the cell
// east of A, and (for B) the cells east/SE. The per-cell shore pass then
// stamps piece22 over A and piece14 over B; piece22 draws water to its tip and
// piece14 puts plain sand on B, so water visibly meets sand (art clash).
//
// Carving B (the bend's inner tip) gives A five water neighbours, which makes
// the continuous piece15 selectable: one 2x2 piece wraps the corner. Two
// mirror directions are handled. Only real water tiles count. No-op when no
// bend is present; running it twice carves nothing the second time.
// ---------------------------------------------------------------------------
void RandomMapGenerator::FillStaircaseBends(int genCode)
{
    const int side = size_.workSide;

    auto realWater = [&](int x, int y) -> bool
    {
        if (x < 0 || y < 0 || x >= side || y >= side)
            return false;
        MapCell* c = cellSlots_[512 * y + x];
        return c != nullptr && IsWaterTile(c);
    };

    // Collect every bend's inner-tip cell first, then carve them as one batch.
    std::vector<std::pair<int, int> > tips;

    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            MapCell* a = (CellExists(x, y)) ? cellSlots_[512 * y + x] : nullptr;
            if (a == nullptr || IsWaterTile(a))
                continue;

            // Direction + : B = (x+1,y+1). Water at NW,N,NE,E of A and at the
            // shared E cell plus E,SE of B.
            if (realWater(x - 1, y - 1) && realWater(x, y - 1) &&
                realWater(x + 1, y - 1) && realWater(x + 1, y) &&
                realWater(x + 2, y + 1) && realWater(x + 2, y + 2))
            {
                MapCell* b = (CellExists(x + 1, y + 1))
                    ? cellSlots_[512 * (y + 1) + (x + 1)] : nullptr;
                if (b != nullptr && !IsWaterTile(b))
                    tips.push_back(std::make_pair(x + 1, y + 1));
            }

            // Direction - (mirror): B = (x-1,y+1). Water at NE,N,NW,W of A and
            // at the shared W cell plus W,SW of B.
            if (realWater(x + 1, y - 1) && realWater(x, y - 1) &&
                realWater(x - 1, y - 1) && realWater(x - 1, y) &&
                realWater(x - 2, y + 1) && realWater(x - 2, y + 2))
            {
                MapCell* b = (CellExists(x - 1, y + 1))
                    ? cellSlots_[512 * (y + 1) + (x - 1)] : nullptr;
                if (b != nullptr && !IsWaterTile(b))
                    tips.push_back(std::make_pair(x - 1, y + 1));
            }
        }
    }

    for (size_t k = 0; k < tips.size(); ++k)
    {
        const int tx = tips[k].first;
        const int ty = tips[k].second;

        // Flatten the tip and make it real water of this body.
        MapCell* tip = cellSlots_[512 * ty + tx];
        tip->Height = 0;
        tip->IsoTileTypeIndex = waterTileIndex_;
        WorkAt(tx, ty).data[14] = genCode;

        // Invalidate the neighbour masks so passes 3/4 recompute (above all
        // the outer-corner land cell A that now sees five water neighbours).
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int nx = tx + dx;
                const int ny = ty + dy;
                if (CellExists(nx, ny))
                    WorkAt(nx, ny).data[16] = -1;
            }
        }

        DiagLog("BEND-FILL tip=(%d,%d) g=%d", tx, ty, genCode);
    }
}

// ---------------------------------------------------------------------------
// FillUnsolvableShoreCorners - 铺岸片前抹掉"2x2 岸片无解"的单格水角
// （移植侧修形，非原版逻辑；判据对原版 6 张山地图 0 命中）。
//
// 病根（截图实证：一格宽沙颈、单格沙菱形扎进深水）：岸片最小是 2x2，下列
// 两种水形里无论怎么选片都盖不全水边——
//   A 型：陆格 a 正交四邻全陆、只在一个对角 d 触水，而拐角片朝对角反方向
//         展开的那一列/行，两格自己又贴着一堵正交深水墙。拐角片的浅水绿
//         边只朝 d，盖不住水墙那一侧，于是沙/草直接接深水。
//   B 型：两个对角相邻的陆格各只在"互相背离"的对角触水（NE 对 SW、NW 对
//         SE），它们选中的两个 2x2 拐角片重叠抢占、绿边朝向相反，先到的
//         落片、后到的第二遍也铺不上。
//
// 处理：把咬得最近的那个"对角水角"填回陆地（每处恰好 1 格）。岸线随即退
// 成 2 格尺度、沙颈变粗，pass 3/4 的 2x2 片就能正常铺。实测本种子只命中
// 两处、共 2 格：A 型填 (47,49)，B 型填 (109,67)。先整批判定位再统一改，
// 填陆按湖体回滚的同款写法（tile 0 占位、Level 归基准、清生成代号），并
// 失效周围 3x3 的邻接掩码缓存，让 pass 3/4 重算。
//
// 时序：FillStaircaseBends 之后、pass 3 选岸片之前。
// ---------------------------------------------------------------------------
void RandomMapGenerator::FillUnsolvableShoreCorners(int genCode)
{
    (void)genCode;
    const int side = size_.workSide;

    auto land = [&](int x, int y) -> bool
    {
        if (x < 0 || y < 0 || x >= side || y >= side)
            return false;
        MapCell* c = cellSlots_[512 * y + x];
        return c != nullptr && !IsWaterTile(c);
    };
    auto water = [&](int x, int y) -> bool
    {
        if (x < 0 || y < 0 || x >= side || y >= side)
            return false;
        MapCell* c = cellSlots_[512 * y + x];
        return c != nullptr && IsWaterTile(c);
    };

    // 候选陆格只取占位格（与 SelectShoreTile 的 mode-0 门同源）。方框内
    // 菱形外的槽为 null，必须先判空，否则解引用即崩。
    auto placeholder = [&](int x, int y) -> bool
    {
        if (x < 0 || y < 0 || x >= side || y >= side)
            return false;
        MapCell* c = cellSlots_[512 * y + x];
        return c != nullptr && IsPlaceholderTile(c);
    };

    // 收集需要填回陆地的"对角水角"坐标（去重）。
    std::vector<std::pair<int, int> > fills;
    auto addFill = [&](int x, int y)
    {
        const std::pair<int, int> key = std::make_pair(x, y);
        if (std::find(fills.begin(), fills.end(), key) == fills.end())
            fills.push_back(key);
    };

    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            if (!placeholder(x, y))
                continue;

            // 正交四邻必须全是陆地。
            if (water(x, y - 1) || water(x + 1, y) ||
                water(x, y + 1) || water(x - 1, y))
                continue;

            const bool nw = water(x - 1, y - 1);
            const bool ne = water(x + 1, y - 1);
            const bool sw = water(x - 1, y + 1);
            const bool se = water(x + 1, y + 1);
            const int diagCount = (nw ? 1 : 0) + (ne ? 1 : 0)
                                + (sw ? 1 : 0) + (se ? 1 : 0);
            if (diagCount != 1)
                continue;

            // ---- B 型：背靠背角岸对。a 为偏南那侧角格，优先处理 ----
            // SW 角格 a=(x,y) 与 NE 角格 b=(x+1,y-1) 配对。
            if (sw && placeholder(x + 1, y - 1) && land(x + 1, y - 1)
                && !water(x + 1, y - 2) && !water(x + 2, y - 1)   // b 的 N/E
                && !water(x + 1, y)     && !water(x, y - 1)       // b 的 S/W
                && water(x + 2, y - 2)                            // b 仅 NE 对角水
                && !water(x, y - 2) && !water(x + 2, y))
            {
                addFill(x - 1, y + 1);  // 填 a 的 SW 水角
                continue;
            }
            // SE 角格 a=(x,y) 与 NW 角格 b=(x-1,y-1) 配对。
            if (se && placeholder(x - 1, y - 1) && land(x - 1, y - 1)
                && !water(x - 1, y - 2) && !water(x, y - 1)       // b 的 N/E
                && !water(x - 1, y)     && !water(x - 2, y - 1)   // b 的 S/W
                && water(x - 2, y - 2)                            // b 仅 NW 对角水
                && !water(x, y - 2) && !water(x - 2, y))
            {
                addFill(x + 1, y + 1);  // 填 a 的 SE 水角
                continue;
            }

            // ---- A 型：拐角片展开侧整列/整行贴着正交深水墙 ----
            if (nw && water(x + 2, y) && water(x + 2, y + 1))
                addFill(x - 1, y - 1);  // 填 NW 水角
            else if (ne && water(x, y + 2) && water(x + 1, y + 2))
                addFill(x + 1, y - 1);  // 填 NE 水角
            else if (sw && water(x + 2, y) && water(x + 2, y + 1))
                addFill(x - 1, y + 1);  // 填 SW 水角
            else if (se && water(x, y - 2) && water(x + 1, y - 2))
                addFill(x + 1, y + 1);  // 填 SE 水角
        }
    }

    // 统一把命中的水角填回陆地（与湖体失败回滚同款还原）。
    for (size_t k = 0; k < fills.size(); ++k)
    {
        const int fx = fills[k].first;
        const int fy = fills[k].second;
        MapCell* c = cellSlots_[512 * fy + fx];
        if (c == nullptr)
            continue;

        c->IsoTileTypeIndex = 0;
        c->Height = 0;
        c->Level = baseLevel_;
        WorkAt(fx, fy).data[14] = 0;
        WorkAt(fx, fy).Byte(75) = 0;

        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (CellExists(fx + dx, fy + dy))
                    WorkAt(fx + dx, fy + dy).data[16] = -1;
            }
        }

        DiagLog("SHORE-CORNER-FILL water=(%d,%d) g=%d", fx, fy, genCode);
    }
}

// ---------------------------------------------------------------------------
// ResetPreviewState - sub_4A8BF0 - DisplayClass foundation-preview reset.
//
// Called once, as the FIRST statement of SmoothWaterBody (0x57a0ce:
// `sub_4A8BF0(&MouseClass::Instance, 0)`). The second argument - the new
// foundation image data - is 0 here, so only the "clear" half runs. Read
// against the YRpp DisplayClass layout:
//
//   this+0x1174  CurrentFoundation_CenterCell     -> foundationPreview_.CenterCell
//   this+0x1178  CurrentFoundation_TopLeftOffset  -> foundationPreview_.TopLeftOffset
//   this+0x117C  CurrentFoundation_Data           -> foundationPreview_.Data
//
// The src_1 == 0 path is exactly three operations, in order:
//
//   1. 0x4a8c16  if CurrentFoundation_Data is non-null, unmark the cells the
//                preview covered: base = CenterCell + TopLeftOffset, then
//                MarkFoundation(base, false) (sub_4A95A0).
//   2. 0x4a8c49  CurrentFoundation_TopLeftOffset := dword_8A03F8, the global
//                default offset. IDA confirms 0x8A03F8 holds {0,0}.
//   3. 0x4a8d1f  CurrentFoundation_Data := nullptr.
//
// Both 16-bit adds in step 1 wrap to 16 bits, so the generator computes the
// sum in int and truncates into CellStruct - matching the vanilla __int16
// arithmetic. Step 2 stores a full 4-byte CellStruct, i.e. it clears both X
// and Y at once.
//
// The src_1 != 0 branch (0x4a8c85 - 0x4a8d11) memcpy's 0x1E0 bytes of
// foundation data into the static buffer dst_, re-points CurrentFoundation_Data
// at it, then derives TopLeftOffset from the foundation bounding size and
// stamps the new cells. It never runs from SmoothWaterBody.
// ---------------------------------------------------------------------------
void RandomMapGenerator::ResetPreviewState()
{
    if (foundationPreview_.Data)
    {
        CellStruct base;
        base.X = static_cast<int16_t>(foundationPreview_.CenterCell.X
                                    + foundationPreview_.TopLeftOffset.X);
        base.Y = static_cast<int16_t>(foundationPreview_.CenterCell.Y
                                    + foundationPreview_.TopLeftOffset.Y);
        MarkFoundation(base, false);
    }

    foundationPreview_.TopLeftOffset = CellStruct{0, 0};
    foundationPreview_.Data = nullptr;
}

// ---------------------------------------------------------------------------
// MarkFoundation - sub_4A95A0 (YRpp DisplayClass::MarkFoundation).
//
//   baseCell = base cell the foundation is anchored at
//   mark     = true to stamp, false to un-stamp
//
// Walks the foundation list at CurrentFoundation_Data (CellStruct entries
// terminated by the sentinel (0x7FFF, 0x7FFF)) and, for each entry, computes
// cell = baseCell + entry and updates that cell:
//
//   if (mark) cell->AltFlags |=  AltCellFlags_ContainsBuilding;
//   else      cell->AltFlags &= ~AltCellFlags_ContainsBuilding;
//
// Two vanilla details:
//   - the routine bails out immediately when the base cell equals the global
//     p_src constant (0x8A03F8 = {0,0});
//   - "legal" is MapClass::CoordinatesLegal (0x568300), whose test
//     `x+y > W' && |x-y| < W' && x+y <= W'+2H'` is exactly CellExists.
// ---------------------------------------------------------------------------
void RandomMapGenerator::MarkFoundation(CellStruct baseCell, bool mark)
{
    if (baseCell.X == 0 && baseCell.Y == 0)
        return;

    for (const CellStruct* entry = foundationPreview_.Data;
         entry->X != 0x7FFF || entry->Y != 0x7FFF;
         ++entry)
    {
        const int x = baseCell.X + entry->X;
        const int y = baseCell.Y + entry->Y;

        if (!CellExists(x, y))
            continue;

        MapCell* cell = cellSlots_[512 * y + x];
        if (mark)
            cell->AltFlags |=  AltCellFlags_ContainsBuilding;
        else
            cell->AltFlags &= ~AltCellFlags_ContainsBuilding;
    }
}

// ---------------------------------------------------------------------------
// TileNeighbourMask - sub_57B210 - 8-direction connectivity mask.
//
// Gate order (0x57b214 - 0x57b2df):
//   1. diamond bounds (W' < x+y <= W'+2H' and |x-y| < W') -> 0 outside;
//   2. mode 1: reject water tile; mode 0: require placeholder tile; mode 2:
//      no tile gate;
//   3. work cell validity byte (+74) must be set, else 0;
//   4. a mask already cached in the work cell's data[16] (>= 0) is returned;
//   5. otherwise scan the 8 raw neighbour slots and build the mask.
//
// Bit <-> direction (read off the slot offsets -513 / -512 / -511 / -1 / +1 /
// +511 / +512 / +513, cross-checked against the corner patterns shared with
// FloodFill and SelectShoreTile):
//   0x40 NW(X-1,Y-1)  0x80 N (X,Y-1)  0x01 NE(X+1,Y-1)
//   0x20 W (X-1,Y)    0x02 E (X+1,Y)
//   0x10 SW(X-1,Y+1)  0x08 S (X,Y+1)  0x04 SE(X+1,Y+1)
//
// A bit is set when the neighbour slot holds a water tile, or when the slot is
// null and the cell itself is water (vanilla v32/v14 = sub_485060(cell)).
// ---------------------------------------------------------------------------
int RandomMapGenerator::TileNeighbourMask(MapCell* cell, int mode)
{
    const int x = cell->MapCoords & 0xFFFF;
    const int y = (uint32_t)cell->MapCoords >> 16;

    if (!CellExists(x, y))
        return 0;

    if (mode == 1)
    {
        const int tile = cell->IsoTileTypeIndex;
        if (tile >= waterTileIndex_ && tile < waterTileIndex_ + 14)
            return 0;
    }
    else if (mode == 0)
    {
        if (!IsPlaceholderTile(cell))
            return 0;
    }

    WorkCell& work = WorkAt(x, y);
    if (work.Byte(74) == 0)
        return 0;
    if (work.data[16] >= 0)
        return work.data[16];

    int mask = 0;
    const bool selfWater = IsWaterTile(cell);
    const int  base = x + (y << 9);

    auto setBit = [&](int index, int bit)
    {
        MapCell* n = RawSlot(index);
        if (n ? IsWaterTile(n) : selfWater)
            mask |= bit;
    };

    setBit(base - 513, 0x40);   // NW
    setBit(base - 512, 0x80);   // N
    setBit(base - 511, 0x01);   // NE
    setBit(base - 1,   0x20);   // W
    setBit(base + 1,   0x02);   // E
    setBit(base + 511, 0x10);   // SW
    setBit(base + 512, 0x08);   // S
    setBit(base + 513, 0x04);   // SE
    return mask;
}

// ---------------------------------------------------------------------------
// FloodFill - sub_57A430 - pass 1, recursive flood fill of one water body.
//
// Signature in the binary is (cell, genCode, flag); every call site passes
// flag = 0. Returns false only when THIS call's body reaches a cell already
// owned by a different, non-zero generation code (old-water pollution). The
// recursive calls' return values are discarded, exactly as in the vanilla body
// (0x57a8d5 has no assignment).
//
// Steps (0x57a435 - 0x57a8de):
//   1. Stamp this cell's work data[16] with its morphology code
//      (TileNeighbourMask(cell, 2)).
//   2. mask = TileNeighbourMask(cell, 0); mask <= 0 -> return true.
//   3. Decide the fall-back / absorb flag (vanilla v44):
//        - mask 11 (0x0B) or 26 (0x1A);
//        - a convex corner: one of 0xA0 / 0x82 / 0x0A / 0x28 set while its
//          two diagonals (0x11 / 0x44) are clear;
//        - axis probes: a connection on one side whose opposite side is still
//          connected one and two cells out (a one-wide bridge).
//   4. Body when the mask runs straight through (N&S 0x88 or W&E 0x22) or the
//      fall-back flag is set:
//        a. owner = work data[14]; owner > 0 && owner != genCode && !flag
//           -> return false (pollution);
//        b. cell->Height = 0, cell->IsoTileTypeIndex = nIdx (water base);
//        c. work data[14] = flag ? 0 : genCode;
//        d. recurse into all 8 in-diamond neighbours (their data[16] reset
//           to -1 first).
//
// Note: the mode-0 placeholder gate is what terminates the recursion - a cell
// converted by step 4b no longer has a placeholder tile, so re-entry returns
// mask 0 and does nothing. That requires nIdx (waterTileIndex_) to be non-zero
// (see the waterTileIndex_ declaration; with nIdx == 0 a converted cell keeps
// the placeholder tile 0 and the fill would not terminate).
// ---------------------------------------------------------------------------
bool RandomMapGenerator::FloodFill(MapCell* cell, int genCode, int flag)
{
    const int cx = cell->MapCoords & 0xFFFF;
    const int cy = (uint32_t)cell->MapCoords >> 16;

    // 1. Stamp the morphology code as the work marker.
    WorkAt(cx, cy).data[16] = TileNeighbourMask(cell, 2);

    // 2. Connectivity mask.
    const int mask = TileNeighbourMask(cell, 0);
    if (mask <= 0)
        return true;

    // 3. Fall-back / absorb flag (vanilla v44).
    bool fallback = (mask == 11 || mask == 26);

    if (((mask & 0xA0) == 0xA0 && (mask & 0x11) == 0)
        || ((mask & 0x82) == 0x82 && (mask & 0x44) == 0)
        || ((mask & 0x0A) == 0x0A && (mask & 0x11) == 0)
        || ((mask & 0x28) == 0x28 && (mask & 0x44) == 0))
        fallback = true;

    // Axis probes: a connection on one side whose opposite side is still
    // connected one and two cells out marks a one-wide bridge.
    if ((mask & 0x20) != 0)   // W set -> probe E and EE, test their E bit
    {
        if ((TileNeighbourMask(CellAt(cx + 1, cy), 0) & 0x02) != 0
            || (TileNeighbourMask(CellAt(cx + 2, cy), 0) & 0x02) != 0)
            fallback = true;
    }
    if ((mask & 0x02) != 0)   // E set -> probe W and WW, test their W bit
    {
        if ((TileNeighbourMask(CellAt(cx - 1, cy), 0) & 0x20) != 0
            || (TileNeighbourMask(CellAt(cx - 2, cy), 0) & 0x20) != 0)
            fallback = true;
    }
    if ((mask & 0x08) != 0)   // S set -> probe N and NN, test their N bit
    {
        if ((TileNeighbourMask(CellAt(cx, cy - 1), 0) & 0x80) != 0
            || (TileNeighbourMask(CellAt(cx, cy - 2), 0) & 0x80) != 0)
            fallback = true;
    }
    if ((mask & 0x80) != 0)   // N set -> probe S and SS, test their S bit
    {
        if ((TileNeighbourMask(CellAt(cx, cy + 1), 0) & 0x08) != 0
            || (TileNeighbourMask(CellAt(cx, cy + 2), 0) & 0x08) != 0)
            fallback = true;
    }

    // 4. Straight-through or absorbed?
    if ((mask & 0x88) != 0x88 && (mask & 0x22) != 0x22 && !fallback)
        return true;

    // 4a. Foreign-water pollution test.
    const int owner = WorkAt(cx, cy).data[14];
    if (owner > 0 && owner != genCode && !flag)
        return false;

    // 4b. Flatten and set the water base tile.
    cell->Height = 0;
    cell->IsoTileTypeIndex = waterTileIndex_;

    // 4c. Write the generation code (flag ? 0 : genCode).
    WorkAt(cx, cy).data[14] = flag ? 0 : genCode;

    // 4d. Recurse into the 8 in-diamond neighbours.
    for (int facing = 0; facing < 8; ++facing)
    {
        MapCell* nb = GetNeighbourCell(cell, facing);
        const int nx = nb->MapCoords & 0xFFFF;
        const int ny = (uint32_t)nb->MapCoords >> 16;
        if (!CellExists(nx, ny))
            continue;
        WorkAt(nx, ny).data[16] = -1;
        FloodFill(nb, genCode, flag);
    }

    return true;
}

// ---------------------------------------------------------------------------
// CleanupTile - sub_57A320 - pass 2, placeholder tile reclaim.
//
// The binary signature is (cell, genCode, flag); the third argument is read
// only as its low byte and the single store it guards (see step 4) never runs
// from SmoothWaterBody, which always passes flag = 0. The declared two-arg
// form is therefore behaviourally exact.
//
// Steps (0x57a320 - 0x57a425, 85 instructions):
//   1. Gate: only cells whose tile lies in [nIdx, nIdx + 12) are considered.
//      NOTE the bound is 12 here, NOT the 14 of IsWaterTile (sub_485060).
//      A cell outside the range returns its tile index unchanged.
//   2. code = TileNeighbourMask(cell, 2) - the cell's morphology code; this
//      is the value the function returns (the caller ignores it).
//   3. When code is one of the eight concave / convex shapes
//      199 / 124 / 241 / 31 / 198 / 108 / 177 / 27, the cell still carries a
//      leftover placeholder tile: it is reclaimed by setting the tile to
//      0xFFFF ("no own tile / covered by a multi-cell tile") and clearing
//      Height to 0.
//   4. (vanilla) when the dead flag's low byte != 0, the generation code is
//      also stamped into the work cell's data[14] (byte +56). All call sites
//      pass flag = 0, so this store is unreachable and is omitted.
//   5. The 8 neighbours are then refreshed so the change propagates into the
//      next pass: for every neighbour inside the diamond (CellExists), its
//      work processing marker data[16] (byte +64) is first cleared to -1 -
//      which forces TileNeighbourMask to recompute instead of reusing a
//      cached mask - and then re-stamped with TileNeighbourMask(nb, 2).
//
// Diamond test uses the same bounds as CellExists, i.e. vanilla globals
// IsoTileTypeIndex_0 (= W') and IsoTileTypeIndex_1 (= W' + 2H').
// ---------------------------------------------------------------------------
int RandomMapGenerator::CleanupTile(MapCell* cell, int genCode)
{
    (void)genCode;   // only used by the unreachable work data[14] store above

    const int tile = cell->IsoTileTypeIndex;
    if (tile < waterTileIndex_ || tile >= waterTileIndex_ + 12)
        return tile;

    const int code = TileNeighbourMask(cell, 2);
    if (code != 199 && code != 124 && code != 241 && code != 31
        && code != 198 && code != 108 && code != 177 && code != 27)
        return code;

    // Reclaim the leftover placeholder tile.
    cell->IsoTileTypeIndex = 0xFFFF;
    cell->Height = 0;

    // Refresh the 8 neighbours' processing markers.
    for (int facing = 0; facing < 8; ++facing)
    {
        MapCell* nb = GetNeighbourCell(cell, facing);
        const int nx = nb->MapCoords & 0xFFFF;
        const int ny = (uint32_t)nb->MapCoords >> 16;
        if (!CellExists(nx, ny))
            continue;

        WorkCell& work = WorkAt(nx, ny);
        work.data[16] = -1;
        work.data[16] = TileNeighbourMask(nb, 2);
    }

    return code;
}

// ---------------------------------------------------------------------------
// SelectShoreTile - sub_57ACF0 - passes 3/4, shore-tile selection.
//
// Binary signature is (this, cell, mode, genCode, flag) - 427 instructions,
// `retn 10h`; only the four stack arguments matter. mode 1 = pass 3 (first
// round), mode 2 = pass 4 (second round).
//
// Decision logic (0x57acf0 - 0x57b127):
//   0. roll = RandomFloatRange(0, 5)  (sub_598030; 0x57ad04). EXACTLY ONE
//      draw per visited cell, so passes 3 + 4 consume 2 * (diamond cell
//      count) draws in iterator order.
//   1. mask = TileNeighbourMask(cell, mode == 2 ? 1 : 0)  (0x57ad12 - 0x57ad26)
//        mode 1 -> mode 0 mask (require placeholder tile)
//        mode 2 -> mode 1 mask (reject water tile)
//      mask == 0 -> return true  (0x57ad2f).
//   2. Derive the shore variant n12:
//
//      mode 2 (0x57ad3e - 0x57ae0a) - four corner combinations, tested in
//      this order:
//        (mask & 0xA0) == 0xA0 -> (mask & 0x11) == 0x11 ? (roll&1)+23
//                                                       : ((mask&1) ? 21 : 30)
//        (mask & 0x82) == 0x82 -> (mask & 0x44) == 0x44 ? (roll&1)+15
//                                                       : ((mask&4) ? 14 : 22)
//        (mask & 0x0A) == 0x0A -> (mask & 0x11) == 0x11 ? (roll&1)+7
//                                                       : ((mask&1) ? 13 : 6)
//        (mask & 0x28) == 0x28 -> (mask & 0x44) == 0x44 ? (roll&1)+31
//                                                       : ((mask&4) ? 5 : 29)
//        none of the four outer tests -> return true.
//
//      mode 1 (0x57ae0f - 0x57b120) - one side bit at a time; the matching
//      branch first walks an edge to count its run length n, then picks the
//      variant from the run's parity plus the neighbour bits:
//        0x02 (probe +Y then +X, walk +Y):
//             n12 = 12  if ((n&1) && !(mask&0x80)) || !(mask&4) || (mask&0x18)
//             else        roll % 3 + 9
//        0x20 (probe +Y then -X, walk +Y):
//             n12 = 28  if ((n&1) && !(mask&0x80)) || !(mask&0x10) || (mask&0x0C)
//             else        roll % 3 + 25
//        0x08 (probe +X then +Y, walk +X):
//             n12 = 4   if (n&1) || !(mask&4) || (mask&3)
//             else        roll % 3 + 1
//        0x80 (probe +X then -Y, walk +X):
//             n12 = 20  if (n&1) || !(mask&1) || (mask&6)
//             else        roll % 3 + 17
//        none of 0x02 / 0x20 / 0x08 / 0x80 -> the LABEL_61 fallback
//        (0x57b0ed): (roll&1)+35 if (mask&1), +33 elif (mask&4),
//        +39 elif (mask&0x10), +37 elif (mask&0x40), else return true.
//   3. n12 <= 0 -> return true  (0x57b125, 0x57b127). Unreachable in practice
//      (every computed n12 is > 0); kept for fidelity.
//
// Placement tail (LABEL_71, 0x57b12d - 0x57b1e3) - implemented:
//     currentBuildingType_ = shorePieces_ + n12 - 1
//         (vanilla Array[ShorePieces + n12 - 1]; only the type's own tile
//          index - its ArrayIndex - ever reaches the map, so that is what we
//          carry)
//     target = cell->MapCoords + shoreAnchor_[n12]
//         (vanilla offset table @ 0xABDB64, paired int16 X/Y, indexed by n12).
//         That table has no writer in the shipped image - it sits in the
//         uninitialised tail of .data and every reference to it is a read - so
//         the offset is 0 and the stamp runs straight into the water on its +X
//         / +Y side. ShoreStampOffset supplies the shift instead.
//     SetFoundationCenter(target)                 (sub_4A91B0)
//     placed = true
//     mode 1 -> PlaceIsoTile(0, 0, cell->Level, genCode, &placed, flag)
//     mode 2 -> PlaceIsoTile(shorePieces_, shorePieces_ + 41,
//                            cell->Level, genCode, &placed, flag)
//     return placed
// The one remaining dependency is PlaceIsoTile (sub_57B440), the shared
// isotile-foundation primitive - see its declaration for why it is still a
// seam. Until it lands the stamp is not performed and the body reports "not
// rejected", keeping the shared pass 3/4 ok-chain (SmoothWaterBody's `ok`)
// intact; the RNG draw of step 0 is consumed in the correct order and count
// either way.
// ---------------------------------------------------------------------------
// Water-side pull-back of a shore stamp, in cells. Defined next to
// kShoreFootprints, which holds the piece sizes it needs.
static CellStruct ShoreStampOffset(int n12, int mask);

bool RandomMapGenerator::SelectShoreTile(MapCell* cell, int mode, int genCode, int flag)
{
    // 0. One RNG draw per visited cell.
    const int roll = rng_.RandomFloatRange(0, 5);

    // Trigger coordinates, used by the shore anchor / placement tail.
    const int cellX = cell->MapCoords & 0xFFFF;
    const int cellY = static_cast<uint32_t>(cell->MapCoords) >> 16;

    // 1. Connectivity mask (mode 1 -> require placeholder, mode 2 -> reject water).
    const int mask = (mode == 2) ? TileNeighbourMask(cell, 1)
                                 : TileNeighbourMask(cell, 0);
    if (mask == 0)                                          // 0x57ad2f
        return true;

    int n12;

    if (mode == 2)
    {
        if ((mask & 0xA0) == 0xA0)
            n12 = ((mask & 0x11) == 0x11) ? (roll & 1) + 23
                                          : ((mask & 1) ? 21 : 30);
        else if ((mask & 0x82) == 0x82)
            n12 = ((mask & 0x44) == 0x44) ? (roll & 1) + 15
                                          : ((mask & 4) ? 14 : 22);
        else if ((mask & 0x0A) == 0x0A)
            n12 = ((mask & 0x11) == 0x11) ? (roll & 1) + 7
                                          : ((mask & 1) ? 13 : 6);
        else if ((mask & 0x28) == 0x28)
            n12 = ((mask & 0x44) == 0x44) ? (roll & 1) + 31
                                          : ((mask & 4) ? 5 : 29);
        else
            return true;                                    // 0x57ae05
    }
    else
    {
        // Walk the edge: from `cell`, step dirA to n1, then dirB to n2, then
        // advance both by dirA while n1 stays land and n2 stays water. The
        // run length counts from 1.
        auto edgeRun = [&](int dirA, int dirB) -> int
        {
            int count = 1;
            MapCell* n1 = GetNeighbourCell(cell, dirA);
            MapCell* n2 = GetNeighbourCell(n1, dirB);
            bool land = !IsWaterTile(n1);
            bool water = IsWaterTile(n2);

            while (land)
            {
                if (!water)
                    break;
                ++count;
                n1 = GetNeighbourCell(n1, dirA);
                n2 = GetNeighbourCell(n2, dirA);
                land = !IsWaterTile(n1);
                water = IsWaterTile(n2);
            }
            return count;
        };

        if ((mask & 0x02) != 0)
        {
            const int n = edgeRun(4, 2);   // +Y then +X
            n12 = (((n & 1) != 0 && (mask & 0x80) == 0)
                   || (mask & 0x04) == 0 || (mask & 0x18) != 0)
                      ? 12 : roll % 3 + 9;
        }
        else if ((mask & 0x20) != 0)
        {
            const int n = edgeRun(4, 6);   // +Y then -X
            n12 = (((n & 1) != 0 && (mask & 0x80) == 0)
                   || (mask & 0x10) == 0 || (mask & 0x0C) != 0)
                      ? 28 : roll % 3 + 25;
        }
        else if ((mask & 0x08) != 0)
        {
            const int n = edgeRun(2, 4);   // +X then +Y
            n12 = (((n & 1) != 0 || (mask & 0x04) == 0 || (mask & 0x03) != 0))
                      ? 4 : roll % 3 + 1;
        }
        else if ((mask & 0x80) != 0)
        {
            const int n = edgeRun(2, 0);   // +X then -Y
            n12 = (((n & 1) != 0 || (mask & 0x01) == 0 || (mask & 0x06) != 0))
                      ? 20 : roll % 3 + 17;
        }
        else
        {
            // LABEL_61 fallback: no side bit matched.
            if ((mask & 0x01) != 0)      n12 = (roll & 1) + 35;
            else if ((mask & 0x04) != 0) n12 = (roll & 1) + 33;
            else if ((mask & 0x10) != 0) n12 = (roll & 1) + 39;
            else if ((mask & 0x40) != 0) n12 = (roll & 1) + 37;
            else                         return true;
        }
    }

    // 2. Reject (vanilla 0x57b125); unreachable as every n12 above is > 0.
    if (n12 <= 0)
        return true;

    // 3. Placement tail (LABEL_71): resolve the shore isotile, move the
    //    foundation anchor to the target cell and stamp it through
    //    PlaceIsoTile (which carries the shore family's geometry).
    currentBuildingType_ = shorePieces_ + n12 - 1;

    const CellStruct pull = ShoreStampOffset(n12, mask);
    const CellStruct target{
        static_cast<int16_t>(cellX + pull.X),
        static_cast<int16_t>(cellY + pull.Y) };
    SetFoundationCenter(target);

    bool placed = true;
    if (mode == 1)
        PlaceIsoTile(0, 0, cell->Level, genCode, &placed, flag);
    else
        PlaceIsoTile(shorePieces_, shorePieces_ + 41, cell->Level,
                     genCode, &placed, flag);

    // [TEMP DIAG] the actual stamping decision for this trigger cell.
    DiagLog("SHORE mode=%d g=%d cell=(%d,%d) mask=0x%02X n12=%d off=(%d,%d) placed=%d",
            mode, genCode, cellX, cellY, mask, n12,
            static_cast<int>(pull.X), static_cast<int>(pull.Y),
            static_cast<int>(placed));

    return placed;
}

// ---------------------------------------------------------------------------
// SetFoundationCenter - sub_4A91B0 - move the foundation-preview center.
//
// Only the CurrentFoundation_Data == null branch is reachable from the
// generation path: ResetPreviewState nulls Data at the start of
// SmoothWaterBody and nothing between then and passes 3/4 ever sets it, so
// this is exactly "return the old center, store the new one" (0x4a92b5 -
// 0x4a92d2):
//     old = CenterCell; CenterCell = target; *out = old;
//
// The Data != null branch (0x4a92e3 - 0x4a94e6) instead unmarks the previous
// footprint with MarkFoundation(base, false), marks the new one, recomputes
// CurrentFoundation_InAdjacent / NoShrouded via sub_4A8EB0 / sub_4A9070 and
// still ends with the same CenterCell store - all DisplayClass-only work.
// The entry special case (0x4a91bd - 0x4a92b1) reads the mouse position when
// the requested target is the (0,0) sentinel; our generator has no mouse and
// no valid diamond cell maps to (0,0), so it cannot fire.
// ---------------------------------------------------------------------------
CellStruct RandomMapGenerator::SetFoundationCenter(CellStruct target)
{
    const CellStruct previous = foundationPreview_.CenterCell;
    foundationPreview_.CenterCell = target;
    return previous;
}

// ---------------------------------------------------------------------------
// PlaceIsoTile - sub_57B440 - isotile foundation stamping, IMPLEMENTED for
// every family whose geometry is tabulated (shore, cliff, cliff/water,
// destroyable cliffs and the waterfall sets); a tile index outside those
// ranges has no image and is a no-op.
//
// Replicates the engine's "place the selected isotile's foundation" routine
// (0x57b440 - 0x57b78d, 257 instructions, `retn 18h`). It is shared: the
// cliff chain (sub_578E60 / sub_578D80 vicinity) reaches it too, so it is
// declared once here and consumed by the SelectShoreTile tail.
//
// Structure (every address below is vanilla's):
//   1. CurrentBuildingType->WhatAmI() must be AbstractType 0x12
//      (IsotileType), else return 0; flag != 0 forces genCode = 0.
//   2. The type's GetImage() (SHP) must be non-null, else return 0.
//   3. Foundation loop: rows [0, type+0x2E8), cols [0, type+0x2E4); for each
//      cell = CurrentFoundation_CenterCell + (col, row), skip outside the
//      diamond (same test as CellExists) and skip slots that are null
//      (falling back to MapClass::InvalidCell with the coords stamped).
//   4. Per-cell value = image[0x10 + 4 * (row * width + col)] - a pointer; a
//      null entry skips the cell.
//   5. Occupancy: value = work data[14] of the cell (sub_5A00C0), and the
//      acceptance rules branch on flag / genCode against the type's own tile
//      index (type+0x294), dword_ABAD28 (shorePieces_) and the two 42-entry
//      variant tables dword_82A7F4 / dword_82A89C; sub_486380 (placeholder),
//      sub_4865B0 (shore), sub_4863D0 (slope/cliff) and sub_578D80 gate the
//      rest. sub_5A0090 writes a generation code into the cell's work
//      data[14].
//   6. Accepted cells get: IsoTileTypeIndex = type+0x294, Height = the
//      foundation cell index, Level = level + imageValue[0x28].
//   7. Any rejection sets *placed = 0 and returns 0.
//
// The footprint width / height (+0x2E4 / +0x2E8) and the per-cell occupancy
// array (+0x10 of the SHP image) come from the tile art, which is shipped in
// the theater .MIX archives. They are measured once and tabulated below; the
// generator carries the geometry, the engine resolves the art by tile index.
//
// Measurements: the 42 "Shore Pieces" variants of the isotile sets. Source is
// the contiguous 42-entry block that starts at offset 0x00B5230 of
// ISOSNOW.MIX, in storage order - cross-checked against XCC Mixer's listing
// (identical widths/heights and identical file order, and the file sizes match
// the block exactly). Both the anchor table dword_ABDB64 and every per-cell Z
// (frame+0x28) are zero for the whole set, so neither needs a table.
//
//   n12  1-3 : 2x2   4: 1x2   5-6: 2x3   7-11: 2x2   12: 2x1
//       13-14: 3x2  15-19: 2x2  20: 1x2  21-22: 2x3  23-27: 2x2
//       28: 2x1     29-30: 3x2  31-40: 2x2  41: 6x4   42: 9x5
// ---------------------------------------------------------------------------

// Footprint of one isotile: the foundation size, the occupied cells (bit
// (row * w + col)) and the per-cell Z offsets (frame+0x28) of the OCCUPIED
// cells in row-major order (empty cells contribute no entry, so the Z array is
// indexed by the occupied-cell counter).
struct IsoFootprint
{
    int                w;
    int                h;
    unsigned long long mask;
    signed char        z[24];
};

// 42 entries, k = tile - shorePieces_ (shore01 .. shore42). Tiles 41 and 42
// are the two irregular pieces; all the others are full rectangles. The whole
// shore set has Z = 0 on every cell, so the Z arrays stay empty.
static const IsoFootprint kShoreFootprints[42] =
{
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 1, 2, 0x3ULL }, { 2, 3, 0x3FULL }, { 2, 3, 0x3FULL },
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 1, 0x3ULL },
    { 3, 2, 0x3FULL }, { 3, 2, 0x3FULL }, { 2, 2, 0xFULL },
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 2, 2, 0xFULL }, { 1, 2, 0x3ULL }, { 2, 3, 0x3FULL },
    { 2, 3, 0x3FULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 2, 1, 0x3ULL }, { 3, 2, 0x3FULL }, { 3, 2, 0x3FULL },
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 2, 2, 0xFULL }, { 2, 2, 0xFULL }, { 2, 2, 0xFULL },
    { 2, 2, 0xFULL }, { 6, 4, 0xFFFFCEULL },
    { 9, 5, 0x1FEFF7FC380CULL },
};

// ---------------------------------------------------------------------------
// ShoreStampOffset - where SelectShoreTile puts the shore stamp's origin.
//
// PlaceIsoTile always grows +X/+Y from the foundation center (cell X =
// CenterCell.X + col, cell Y = CenterCell.Y + row), so a piece of size w x h
// anchored at the trigger cell covers [x, x+w-1] x [y, y+h-1]. Water inside
// that rectangle is fatal: the per-cell acceptance gate rejects it, and the
// first rejection abandons the whole footprint (PlaceIsoTile returns on the
// spot), which leaves the water-adjacent cell bare.
//
// The shipped image's own anchor table (0xABDB64) would place the shift, but
// it sits in the uninitialised tail of .data with no writer anywhere in the
// binary, so it is all zero and the piece runs straight into the water on its
// +X / +Y side. This helper supplies the shift instead.
//
// Two anchors are tried, in this order:
//
// 1. The piece's own facing (ShoreFacesEast / ShoreFacesSouth): the art is
//    drawn against the water on one edge, so the trigger cell has to end up on
//    that edge and the stamp - which only grows +X/+Y - must be pulled back by
//    w-1 in X and/or h-1 in Y.  Used only when that rectangle is water-clear.
//    The search below happens to return the same anchor for every piece whose
//    water is on a SIDE, but not for the corner (diagonal) groups NE 35/36,
//    SW 39/40 and SE 33/34: the water they must dodge is on a corner the
//    zero-shift rectangle never covers, so the search stops one step short and
//    the corner art ends up painted one row/column into the land - over cells
//    that then keep the corner art even though they have water on a side of
//    their own (the bend defect).  This anchor is what fixes those.
//
// 2. Otherwise it looks at the four placements the trigger allows - no shift,
//    either single shift, both - and keeps the LEAST displaced one whose
//    rectangle holds none of the water cells the mask names.
//    Least-displacement matters: shifting further than the water requires
//    pushes the origin onto a cell an earlier piece has already stamped, which
//    is the failure the rectangle check alone would miss.
//    Ties (w == h, both single shifts clear) go to the X shift by loop order.
//
// `mask` is TileNeighbourMask's: NE 0x01, E 0x02, SE 0x04, S 0x08, SW 0x10,
// W 0x20, NW 0x40, N 0x80.
// ---------------------------------------------------------------------------

// Does the rectangle [dx, dx+w-1] x [dy, dy+h-1], taken relative to the
// trigger cell, cover any of the trigger's water neighbours?
static bool FootprintCoversWater(int mask, int dx, int dy, int w, int h)
{
    static const struct { int bit; int ox; int oy; } kNeighbours[8] =
    {
        { 0x80,  0, -1 }, { 0x01,  1, -1 }, { 0x02,  1,  0 }, { 0x04,  1,  1 },
        { 0x08,  0,  1 }, { 0x10, -1,  1 }, { 0x20, -1,  0 }, { 0x40, -1, -1 },
    };

    for (int i = 0; i < 8; ++i)
    {
        if ((mask & kNeighbours[i].bit) == 0)
            continue;
        const int ox = kNeighbours[i].ox;
        const int oy = kNeighbours[i].oy;
        if (ox >= dx && ox <= dx + w - 1 && oy >= dy && oy <= dy + h - 1)
            return true;
    }
    return false;
}

// Which edge of the piece its art is drawn against water on, read straight off
// the n12 dispatch in SelectShoreTile: a group only reaches the n12 it names
// when the trigger cell has water on that side, so the group itself says which
// side the piece faces. 6..16 and 22 come from the E-bearing branches (E,
// N+E, E+S) and 33..36 from the NE / SE fallbacks; 1..8, 13, 29, 31..34 and
// 39/40 come from the S-bearing ones (S, S+W, E+S) and the SW / SE fallbacks.
static bool ShoreFacesEast(int n12)
{
    switch (n12)
    {
    case 6:  case 7:  case 8:  case 9:  case 10: case 11: case 12: case 13:
    case 14: case 15: case 16: case 22:
    case 33: case 34: case 35: case 36:
        return true;
    default:
        return false;
    }
}

static bool ShoreFacesSouth(int n12)
{
    switch (n12)
    {
    case 1:  case 2:  case 3:  case 4:  case 5:  case 6:  case 7:  case 8:
    case 13: case 29: case 31: case 32: case 33: case 34:
    case 39: case 40:
        return true;
    default:
        return false;
    }
}

static CellStruct ShoreStampOffset(int n12, int mask)
{
    CellStruct offset;
    offset.X = 0;
    offset.Y = 0;

    const int idx = n12 - 1;
    if (idx < 0 || idx >= 42)
        return offset;

    const int w = kShoreFootprints[idx].w;
    const int h = kShoreFootprints[idx].h;

    // The anchor the piece's art asks for: the trigger cell sits on the piece's
    // water edge, so the stamp (which only grows +X/+Y) has to be pulled back
    // to keep its far edge there.
    CellStruct facing;
    facing.X = ShoreFacesEast(n12) ? static_cast<int16_t>(-(w - 1)) : 0;
    facing.Y = ShoreFacesSouth(n12) ? static_cast<int16_t>(-(h - 1)) : 0;
    if ((facing.X != 0 || facing.Y != 0)
        && !FootprintCoversWater(mask, facing.X, facing.Y, w, h))
        return facing;

    int bestScore = 0;
    bool found = false;

    for (int iy = 0; iy < 2; ++iy)
    {
        for (int ix = 0; ix < 2; ++ix)
        {
            const int dx = (ix != 0) ? -(w - 1) : 0;
            const int dy = (iy != 0) ? -(h - 1) : 0;
            if (FootprintCoversWater(mask, dx, dy, w, h))
                continue;

            const int score = ((dx < 0) ? -dx : dx) + ((dy < 0) ? -dy : dy);
            if (!found || score < bestScore)
            {
                found = true;
                bestScore = score;
                offset.X = static_cast<int16_t>(dx);
                offset.Y = static_cast<int16_t>(dy);
            }
        }
    }

    // Water on both sides of a piece this wide: no placement is clear, so fall
    // back to the trigger cell itself, exactly as a zero anchor would.
    return offset;
}

// Waterfall pieces - GenerateDelta's river-mouth dressing. Measured from the
// released w-a- .. w-d- tiles: both theaters share the same geometry (only the
// file sizes differ). Family order matches the delta's direction switch and the
// nIdx_0..3 mapping: [0] East = W-b-, [1] West = W-d-, [2] South = W-a-,
// [3] North = W-c-. The four pieces are -01 .. -04, -02 being the thin one.
static const IsoFootprint kWaterfallFootprints[4][4] =
{
    {   // [0] East - W-b-
        { 4, 2, 0xFFULL, { 4, 4, 0, 0, 4, 4, 0, 0 } },
        { 4, 1, 0xFULL,  { 4, 4, 0, 0 } },
        { 4, 2, 0xFFULL, { 4, 4, 0, 0, 4, 4, 0, 0 } },
        { 4, 2, 0xFFULL, { 4, 4, 0, 0, 4, 4, 0, 0 } }
    },
    {   // [1] West - W-d-
        { 2, 2, 0xFULL, { 4, 4, 4, 4 } },
        { 2, 1, 0x3ULL, { 4, 4 } },
        { 2, 2, 0xFULL, { 4, 4, 4, 4 } },
        { 2, 2, 0xFULL, { 4, 4, 4, 4 } }
    },
    {   // [2] South - W-a-
        { 2, 4, 0xFFULL, { 4, 4, 4, 4, 0, 0, 0, 0 } },
        { 1, 4, 0xFULL,  { 4, 4, 0, 0 } },
        { 2, 4, 0xFFULL, { 4, 4, 4, 4, 0, 0, 0, 0 } },
        { 2, 4, 0xFFULL, { 4, 4, 4, 4, 0, 0, 0, 0 } }
    },
    {   // [3] North - W-c-
        { 2, 2, 0xFULL, { 4, 4, 4, 4 } },
        { 1, 2, 0x3ULL, { 4, 4 } },
        { 2, 2, 0xFULL, { 4, 4, 4, 4 } },
        { 2, 2, 0xFULL, { 4, 4, 4, 4 } }
    },
};

// Cliff set - 40 pieces, k = tile - shoreTileIndex_ (CliffSet). Measured from
// the released cliff01 .. cliff40 tiles; cliff41/42 and cliff33a/34a exist as
// extra art but lie outside TilesInSet = 40, so they carry no tile index.
static const IsoFootprint kCliffFootprints[40] =
{
    { 2, 3, 0x2DULL, { 4, 0, 4, 0 } },        // cliff01
    { 1, 2, 0x3ULL,  { 4, 0 } },              // cliff02
    { 2, 3, 0x2DULL, { 4, 0, 4, 0 } },        // cliff03
    { 2, 3, 0x2DULL, { 4, 0, 4, 0 } },        // cliff04
    { 2, 2, 0xFULL,  { 4, 4, 0, 0 } },        // cliff05
    { 2, 2, 0xFULL,  { 4, 4, 0, 0 } },        // cliff06
    { 2, 2, 0xFULL,  { 4, 4, 0, 0 } },        // cliff07
    { 1, 2, 0x3ULL,  { 4, 0 } },              // cliff08
    { 2, 2, 0x7ULL,  { 4, 0, 0 } },           // cliff09
    { 2, 2, 0x7ULL,  { 4, 0, 0 } },           // cliff10
    { 2, 2, 0x7ULL,  { 4, 0, 0 } },           // cliff11
    { 1, 1, 0x1ULL,  { 0 } },                 // cliff12
    { 1, 1, 0x1ULL,  { 0 } },                 // cliff13
    { 1, 1, 0x1ULL,  { 0 } },                 // cliff14
    { 2, 2, 0xFULL,  { 4, 0, 4, 0 } },        // cliff15
    { 2, 2, 0xFULL,  { 4, 0, 4, 0 } },        // cliff16
    { 2, 2, 0xFULL,  { 4, 0, 4, 0 } },        // cliff17
    { 2, 1, 0x3ULL,  { 4, 0 } },              // cliff18
    { 3, 2, 0x33ULL, { 4, 0, 4, 0 } },        // cliff19
    { 3, 2, 0x33ULL, { 4, 0, 4, 0 } },        // cliff20
    { 3, 2, 0x33ULL, { 4, 0, 4, 0 } },        // cliff21
    { 2, 1, 0x3ULL,  { 4, 0 } },              // cliff22
    { 2, 1, 0x3ULL,  { 4, 4 } },              // cliff23
    { 2, 1, 0x3ULL,  { 4, 4 } },              // cliff24
    { 2, 1, 0x3ULL,  { 4, 4 } },              // cliff25
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff26
    { 2, 2, 0x7ULL,  { 4, 4, 4 } },           // cliff27
    { 2, 2, 0xEULL,  { 4, 4, 4 } },           // cliff28
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff29
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff30
    { 2, 2, 0x7ULL,  { 4, 4, 4 } },           // cliff31
    { 2, 2, 0xEULL,  { 4, 4, 4 } },           // cliff32
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff33
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff34
    { 1, 2, 0x3ULL,  { 4, 4 } },              // cliff35
    { 1, 2, 0x3ULL,  { 4, 4 } },              // cliff36
    { 1, 2, 0x3ULL,  { 4, 4 } },              // cliff37
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff38
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff39
    { 1, 1, 0x1ULL,  { 4 } },                 // cliff40
};

// Cliff/Water pieces - 28, k = tile - waterCliffsIndex_ (WaterCliffs).
static const IsoFootprint kCliffWaterFootprints[28] =
{
    { 2, 3, 0x2DULL, { 4, 0, 4, 0 } },        // wcliff01
    { 1, 2, 0x3ULL,  { 4, 0 } },              // wcliff02
    { 2, 3, 0x2DULL, { 4, 0, 4, 0 } },        // wcliff03
    { 2, 3, 0x2DULL, { 4, 0, 4, 0 } },        // wcliff04
    { 2, 2, 0xFULL,  { 4, 4, 0, 0 } },        // wcliff05
    { 2, 2, 0xFULL,  { 4, 4, 0, 0 } },        // wcliff06
    { 2, 2, 0xFULL,  { 4, 4, 0, 0 } },        // wcliff07
    { 1, 2, 0x3ULL,  { 4, 0 } },              // wcliff08
    { 2, 2, 0x7ULL,  { 4, 0, 0 } },           // wcliff09
    { 2, 2, 0xFULL,  { 4, 0, 0, 0 } },        // wcliff10
    { 2, 2, 0x7ULL,  { 4, 0, 0 } },           // wcliff11
    { 1, 1, 0x1ULL,  { 0 } },                 // wcliff12
    { 1, 1, 0x1ULL,  { 0 } },                 // wcliff13
    { 1, 1, 0x1ULL,  { 0 } },                 // wcliff14
    { 2, 2, 0xFULL,  { 4, 0, 4, 0 } },        // wcliff15
    { 2, 2, 0xFULL,  { 4, 0, 4, 0 } },        // wcliff16
    { 2, 2, 0xFULL,  { 4, 0, 4, 0 } },        // wcliff17
    { 2, 1, 0x3ULL,  { 4, 0 } },              // wcliff18
    { 3, 2, 0x33ULL, { 4, 0, 4, 0 } },        // wcliff19
    { 3, 2, 0x33ULL, { 4, 0, 4, 0 } },        // wcliff20
    { 3, 2, 0x33ULL, { 4, 0, 4, 0 } },        // wcliff21
    { 2, 1, 0x3ULL,  { 4, 0 } },              // wcliff22
    { 2, 3, 0x3DULL, { 4, 0, 4, 0, 0 } },     // wcliff23
    { 1, 3, 0x7ULL,  { 0, 0, 0 } },           // wcliff24
    { 3, 1, 0x7ULL,  { 0, 0, 0 } },           // wcliff25
    { 2, 2, 0xFULL,  { 4, 0, 0, 0 } },        // wcliff26
    { 2, 2, 0xFULL,  { 4, 0, 0, 0 } },        // wcliff27
    { 3, 2, 0x37ULL, { 4, 0, 0, 4, 0 } },     // wcliff28
};

// Destroyable Cliffs - 2, k = tile - destroyableCliffsIndex_.
static const IsoFootprint kDestroyableCliffFootprints[2] =
{
    { 6, 4, 0x7BFFDEULL,
      { 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },  // dcliff01
    { 4, 6, 0x6FFFF6ULL,
      { 4, 0, 4, 4, 0, 0, 4, 4, 0, 0, 4, 4, 0, 0, 4, 4, 0, 0, 4, 0 } },  // dcliff02
};

// The footprint of one isotile, resolved from the family tables by the very
// rule PlaceIsoTile applies: the shore set (42), the cliff set (40), the
// cliff/water pieces (28), the destroyable cliffs (2) and the four waterfall
// sets (4 each). nullptr for a tile that is none of them.
//
// sub_5A6C10 (PlaceWaterDetailTile) stamps whatever tile it is handed, so it
// needs the same lookup the foundation stamp uses - that is what this helper
// is for.
static const IsoFootprint* FindFamilyFootprint(
    int tile, int shorePieces, int shoreTileIndex, int waterCliffsIndex,
    int destroyableCliffsIndex, const int waterFamily4Base[4])
{
    if (shorePieces >= 0 && tile >= shorePieces && tile < shorePieces + 42)
        return &kShoreFootprints[tile - shorePieces];
    if (shoreTileIndex >= 0 && tile >= shoreTileIndex && tile < shoreTileIndex + 40)
        return &kCliffFootprints[tile - shoreTileIndex];
    if (waterCliffsIndex >= 0 && tile >= waterCliffsIndex &&
        tile < waterCliffsIndex + 28)
        return &kCliffWaterFootprints[tile - waterCliffsIndex];
    if (destroyableCliffsIndex >= 0 && tile >= destroyableCliffsIndex &&
        tile < destroyableCliffsIndex + 2)
        return &kDestroyableCliffFootprints[tile - destroyableCliffsIndex];
    for (int f = 0; f < 4; ++f)
    {
        if (waterFamily4Base[f] >= 0 && tile >= waterFamily4Base[f] &&
            tile < waterFamily4Base[f] + 4)
            return &kWaterfallFootprints[f][tile - waterFamily4Base[f]];
    }
    return nullptr;
}

// The two 42-entry variant tables sub_57B440 consults when an existing shore
// piece and the one being placed share a cell: dword_82A7F4 @0x82A7F4 ("side"
// id per shore variant) and dword_82A89C @0x82A89C (turn id), both int[42] read
// straight out of the image.
static const int kShoreVariantSide[42] =
{
    0, 0, 0, 1, 2, 3, 4, 4, 5, 5, 5, 6, 7, 8, 9, 9, 10, 10, 10, 11, 12,
    13, 14, 14, 15, 15, 15, 16, 17, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23,
    23, 24, 25
};
static const int kShoreVariantTurn[42] =
{
    4, 4, 4, 4, 4, 4, 3, 3, 2, 2, 2, 2, 2, 2, 1, 1, 0, 0, 0, 0, 0,
    0, 7, 7, 6, 6, 6, 6, 6, 6, 5, 5, 3, 3, 1, 1, 7, 7, 5, 5, 4, 4
};

// Family bases for the two gate helpers below (all filled from the theater INI;
// -1 = the key was missing, which the helpers treat as "family absent").
struct TileFamilyBases
{
    int cliff;          // CliffSet          (40 wide)
    int waterCliffs;    // WaterCliffs       (28 wide)
    int destroyable;    // DestroyableCliffs  (2 wide)
    int ramps;          // CliffRamps        (20 wide)
    int caves;          // WaterCaves         (4 wide)
    int bridge;         // BridgeSet         (16 wide)
    int woodBridge;     // WoodBridgeSet     (16 wide)
    int waterfall[4];   // East, West, South, North (4 each)
};

// sub_578D80 (0x578D80) - may `tile` be placed on foundation cell `h`?
// The four waterfall families only restrict their FIRST and LAST piece, by the
// cell's position inside the foundation.
static bool TileFitsFoundation(const TileFamilyBases& b, int tile, int h)
{
    if (b.cliff >= 0 && tile >= b.cliff && tile < b.cliff + 40)
        return true;                                     // 0x578d8f
    if (b.waterfall[0] >= 0 && tile >= b.waterfall[0] && tile < b.waterfall[0] + 4)
        return (tile == b.waterfall[0] || tile == b.waterfall[0] + 3)
                   ? (h != 0 && h != 4) : true;          // East  0x578dae
    if (b.waterfall[1] >= 0 && tile >= b.waterfall[1] && tile < b.waterfall[1] + 4)
        return (tile == b.waterfall[1] || tile == b.waterfall[1] + 3)
                   ? (h != 1 && h != 3) : true;          // West  0x578de2
    if (b.waterfall[2] >= 0 && tile >= b.waterfall[2] && tile < b.waterfall[2] + 4)
        return (tile == b.waterfall[2] || tile == b.waterfall[2] + 3)
                   ? (h >= 2) : true;                    // South 0x578e0b
    if (b.waterfall[3] >= 0 && tile >= b.waterfall[3] && tile < b.waterfall[3] + 4)
        return (tile == b.waterfall[3] || tile == b.waterfall[3] + 3)
                   ? (h != 2 && h != 3) : true;          // North 0x578e33
    return b.ramps >= 0 && tile >= b.ramps && tile < b.ramps + 20;  // 0x578d93
}

// sub_4863D0 (0x4863D0) - does the cell already carry a tile of one of the
// cliff-like families? Same waterfall arms as above, but read off the CELL
// (tile at +0x38, Height at +0x11A).
static bool IsCliffFamilyCell(const TileFamilyBases& b, const MapCell* cell)
{
    const int t = cell->IsoTileTypeIndex;
    if (b.cliff >= 0 && t >= b.cliff && t < b.cliff + 40)
        return true;                                     // 0x4863dc
    if (b.waterfall[0] >= 0 && t >= b.waterfall[0] && t < b.waterfall[0] + 4)
        return (t == b.waterfall[0] || t == b.waterfall[0] + 3)
                   ? (cell->Height != 0 && cell->Height != 4) : true;
    if (b.waterfall[1] >= 0 && t >= b.waterfall[1] && t < b.waterfall[1] + 4)
        return (t == b.waterfall[1] || t == b.waterfall[1] + 3)
                   ? (cell->Height != 1 && cell->Height != 3) : true;
    if (b.waterfall[2] >= 0 && t >= b.waterfall[2] && t < b.waterfall[2] + 4)
        return (t == b.waterfall[2] || t == b.waterfall[2] + 3)
                   ? (cell->Height >= 2) : true;
    if (b.waterfall[3] >= 0 && t >= b.waterfall[3] && t < b.waterfall[3] + 4)
        return (t == b.waterfall[3] || t == b.waterfall[3] + 3)
                   ? (cell->Height != 2 && cell->Height != 3) : true;
    if (b.ramps >= 0 && t >= b.ramps && t < b.ramps + 20) return true;
    if (b.caves >= 0 && t >= b.caves && t < b.caves + 4) return true;
    if (b.bridge >= 0 && t >= b.bridge && t < b.bridge + 16) return true;
    if (b.woodBridge >= 0 && t >= b.woodBridge && t < b.woodBridge + 16) return true;
    if (b.destroyable >= 0 && t >= b.destroyable && t < b.destroyable + 2) return true;
    return b.waterCliffs >= 0 && t >= b.waterCliffs && t < b.waterCliffs + 28;
}

bool RandomMapGenerator::PlaceIsoTile(int lo, int hi, int level, int genCode,
                                      bool* placed, int flag)
{
    // Vanilla also asserts CurrentBuildingType->WhatAmI() == 0x12
    // (IsotileType) and a non-null GetImage(); here the family tables stand in
    // for the image, so a tile with no tabulated geometry cannot be placed.
    if (flag != 0)
        genCode = 0;                                  // 0x57b463

    // 1. Geometry lookup by tile index. Tabulated: the shore set (42), the
    //    cliff set (40), the cliff/water pieces (28), the destroyable cliffs
    //    (2) and the four waterfall sets (4 each). A -1 base (missing INI key)
    //    never matches, and the lookup is by explicit range so a tile of one
    //    family can never borrow another family's table.
    const int tile = currentBuildingType_;
    const IsoFootprint* fp = nullptr;
    if (shorePieces_ >= 0 && tile >= shorePieces_ && tile < shorePieces_ + 42)
        fp = &kShoreFootprints[tile - shorePieces_];
    else if (shoreTileIndex_ >= 0 && tile >= shoreTileIndex_ &&
             tile < shoreTileIndex_ + 40)
        fp = &kCliffFootprints[tile - shoreTileIndex_];
    else if (waterCliffsIndex_ >= 0 && tile >= waterCliffsIndex_ &&
             tile < waterCliffsIndex_ + 28)
        fp = &kCliffWaterFootprints[tile - waterCliffsIndex_];
    else if (destroyableCliffsIndex_ >= 0 && tile >= destroyableCliffsIndex_ &&
             tile < destroyableCliffsIndex_ + 2)
        fp = &kDestroyableCliffFootprints[tile - destroyableCliffsIndex_];
    else
    {
        for (int f = 0; f < 4 && fp == nullptr; ++f)
        {
            if (waterFamily4Base_[f] >= 0 && tile >= waterFamily4Base_[f] &&
                tile < waterFamily4Base_[f] + 4)
                fp = &kWaterfallFootprints[f][tile - waterFamily4Base_[f]];
        }
    }
    if (fp == nullptr)
        return false;                                 // no image: 0x57b486
    if (fp->h <= 0)
        return true;                                  // 0x57b49c

    TileFamilyBases families;
    families.cliff       = shoreTileIndex_;           // CliffSet
    families.waterCliffs = waterCliffsIndex_;
    families.destroyable = destroyableCliffsIndex_;
    families.ramps       = cliffRampsIndex_;
    families.caves       = waterCavesIndex_;
    families.bridge      = bridgeSetIndex_;
    families.woodBridge  = woodBridgeSetIndex_;
    for (int f = 0; f < 4; ++f)
        families.waterfall[f] = waterFamily4Base_[f];

    const int cx = foundationPreview_.CenterCell.X;
    const int cy = foundationPreview_.CenterCell.Y;
    int zIndex = 0;                                   // occupied-cell counter

    // 2. Foundation loop (0x57b4a2): rows [0, h) then cols [0, w). The image
    //    entry for a cell is the occupied mask; cells without one are skipped
    //    (vanilla: a null frame pointer).
    for (int row = 0; row < fp->h; ++row)
    {
        for (int col = 0; col < fp->w; ++col)
        {
            if (((fp->mask >> (row * fp->w + col)) & 1ULL) == 0)
                continue;                             // frame == null
            const int z = fp->z[zIndex];
            ++zIndex;

            const int x = cx + col;
            const int y = cy + row;
            if (!CellExists(x, y))
                continue;                             // 0x57b516
            MapCell*  cell = cellSlots_[512 * y + x];
            const int h = col + row * fp->w;          // 0x57b555

            // 3. Occupancy preamble (0x57b571 - 0x57b628), driven by the work
            //    cell's generation code (sub_5A00C0 reads, sub_5A0090 marks).
            //    flag forces the code to 0, so only the second variant table is
            //    consulted.
            WorkCell& work = workCells_[x + size_.workSide * y];
            const int wc = (flag != 0) ? 0 : work.data[14];

            // Port extension: record per-footprint-cell stamp/block outcomes for
            // CliffSet pieces so RepairCliffPieces can fix swallowed pieces.
            const auto recordCliff = [&](int okx)
            {
                if (genCode == -1 && shoreTileIndex_ >= 0
                    && tile >= shoreTileIndex_ && tile < shoreTileIndex_ + 40)
                {
                    cliffStamps_.push_back(
                        { static_cast<int16_t>(cx), static_cast<int16_t>(cy),
                          static_cast<int16_t>(x), static_cast<int16_t>(y),
                          static_cast<uint8_t>(tile - shoreTileIndex_ + 1),
                          static_cast<uint8_t>(h),
                          static_cast<int8_t>(z),
                          static_cast<uint8_t>(okx) });
                }
            };

            if (flag == 0 && wc > 0 && wc != genCode && genCode != -1)
            {
                if (!IsPlaceholderTile(cell))
                {
                    const int nOld = cell->IsoTileTypeIndex - shorePieces_;
                    const int nNew = tile - shorePieces_;
                    if (nOld < 0 || nNew < 0 || nOld > 41 || nNew > 41 ||
                        kShoreVariantSide[nOld] != kShoreVariantSide[nNew])
                    {
                        if (placed)
                            *placed = false;          // 0x57b718
                        return false;
                    }
                    return true;                      // 0x57b703
                }
                work.data[14] = genCode;              // 0x57b623
            }
            else if (wc == genCode)                   // LABEL_19 -> LABEL_20
            {
                const int nOld = cell->IsoTileTypeIndex - shorePieces_;
                const int nNew = tile - shorePieces_;
                if (nOld >= 0 && nNew >= 0 && nOld <= 41 && nNew <= 41)
                {
                    int d = kShoreVariantTurn[nOld] - kShoreVariantTurn[nNew];
                    if (d < 0)
                        d = -d;
                    if (d >= 3 && d <= 5)
                    {
                        if (placed)
                            *placed = false;          // 0x57b72b
                        return false;
                    }
                }
            }
            else if (!IsPlaceholderTile(cell))
            {
                if (genCode != -1)
                {
                    if (placed)
                        *placed = false;              // LABEL_58 / 0x57b77d
                    return false;
                }
                if (shoreTileIndex_ >= 0 && tile >= shoreTileIndex_
                    && tile < shoreTileIndex_ + 40)
                    recordCliff(0);
                continue;                             // LABEL_36: leave it be
            }
            else
            {
                work.data[14] = genCode;              // 0x57b6cc
            }

            // 4. LABEL_32 - the per-cell acceptance gate (0x57b64a): the stamp
            //    only replaces placeholder cells, or cells already carrying a
            //    tile inside the caller's [lo, hi] range.
            if (IsPlaceholderTile(cell) ||
                (cell->IsoTileTypeIndex >= lo && cell->IsoTileTypeIndex <= hi))
            {
                // The Height byte names a cell inside the tile's OWN image, and
                // it has to be one that exists - RecalcAttributes wipes the cell
                // (tile -> 0xFFFF, Height -> 0, Slope -> 0) as soon as
                // TileCellHasFrame(tile, Height) answers false.
                //
                // The footprint's cell index is not that number: RA2's cliff,
                // shore and waterfall art is one cell per TMP (every clat*.tem
                // in the theater is CellsInX = CellsInY = 1), while the
                // footprints here are 2 x 2 up to 2 x 3. Writing h verbatim
                // therefore pointed at cells the art does not have and every
                // fresh cliff/ramp tile was wiped again on the next recalc.
                // Vanilla writes the index inside the tile's CellsInX x CellsInY
                // image grid (sub_57B440: mov [esi+11Ah], bl), so keep h only
                // when the tile really has that cell.
                int subCell = h;
                if (const TileCellAttr* first = TileCellAttrAt(tile, 0))
                {
                    const int cells = first->cellsInX * first->cellsInY;
                    if (cells > 0 && h >= cells)
                        subCell = 0;
                }

                cell->IsoTileTypeIndex = tile;
                cell->Height           = subCell;
                cell->Level            = level + z;
                if (shoreTileIndex_ >= 0 && tile >= shoreTileIndex_ &&
                    tile < shoreTileIndex_ + 40)
                    recordCliff(1);
                continue;                             // LABEL_36
            }

            // 5. Rejection (0x57b774): a cliff-like existing tile that cannot
            //    take this piece clears the caller's success flag.
            const bool existingIsShore =
                (cell->IsoTileTypeIndex >= shorePieces_ &&
                 cell->IsoTileTypeIndex < shorePieces_ + 42);
            if ((existingIsShore && TileFitsFoundation(families, tile, h)) ||
                (tile >= shorePieces_ && tile < shorePieces_ + 42 &&
                 IsCliffFamilyCell(families, cell)))
            {
                if (placed)
                    *placed = false;                  // 0x57b77d
            }
            if (shoreTileIndex_ >= 0 && tile >= shoreTileIndex_
                && tile < shoreTileIndex_ + 40)
                recordCliff(0);
            return false;                             // 0x57b6aa
        }
    }

    if (placed)
        *placed = true;
    return true;                                      // 0x57b6b3
}

// ---------------------------------------------------------------------------
// RepairCliffPieces - PORT-ONLY post pass (runs right after CorrectCliffTiles).
//
// Westwood's PlaceCliffs has two unfixed corner cases left in the final map:
//
//  A. A multi-cell CliffSet piece has footprint cells that were already taken
//     by a CliffRamp piece (carved earlier by the region ramp builder) or by
//     another CliffSet piece. PlaceIsoTile skips those cells (or aborts the
//     whole stamp midway), so the multi-cell art is left half-rendered:
//       - a 2x2 wall (slots 4-7) whose EAST column is blocked collapses to the
//         1x2 vertical wall strip C8 (slot 8, t56);
//       - every other damaged piece is taken apart into single cells: the
//         z=4 wall cell becomes the 1x1 inner corner C34 (slot 34, t82) and
//         each z=0 foot cell becomes one of the flat caps C12..C14
//         (slots 12-14, t60..t62).
//
// Only art (IsoTileTypeIndex / Height) is changed; Level, marks, SlopeIndex
// and the RNG stream are never touched.
// ---------------------------------------------------------------------------
void RandomMapGenerator::RepairCliffPieces()
{
    if (shoreTileIndex_ < 0)
    {
        cliffStamps_.clear();
        return;
    }

    const auto isCliffTile = [&](int t)
    {
        return t >= shoreTileIndex_ && t < shoreTileIndex_ + 40;
    };

    // CorrectCliffTiles swaps pieces inside these slot clusters, so a surviving
    // cell of a damaged piece may carry any cluster mate afterwards. Arguments
    // are 0-BASED family indices (tile - shoreTileIndex_, 0..39):
    //   slots 4-6  -> 3..5   slots 8-10 -> 7..9    slots 11-13 -> 10..12
    //   slots 14-16-> 13..15 slots 22-24 -> 21..23 slots 34-36 -> 33..35
    const auto clusterOf = [](int s) -> int
    {
        if (s >= 3  && s <= 5)  return 1;
        if (s >= 7  && s <= 9)  return 2;
        if (s >= 10 && s <= 12) return 3;
        if (s >= 13 && s <= 15) return 4;
        if (s >= 21 && s <= 23) return 5;
        if (s >= 33 && s <= 35) return 6;
        return 0;
    };

    const auto stillHoldsPiece = [&](MapCell* c, int slot1)
    {
        if (c == nullptr || !isCliffTile(c->IsoTileTypeIndex))
            return false;
        const int idx = c->IsoTileTypeIndex - shoreTileIndex_;
        // Slots 4-7 (1-based) end up as indices 3..6 (slot 7 never swaps);
        // every other damaged piece only needs its own swap cluster to match.
        if (slot1 >= 4 && slot1 <= 7)
            return idx >= 3 && idx <= 6;
        return clusterOf(idx) == clusterOf(slot1 - 1);
    };

    const auto writeTile = [&](int x, int y, int tile, int height)
    {
        if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
            return;
        MapCell* c = CellAt(static_cast<int16_t>(x), static_cast<int16_t>(y));
        DiagLog("CLIFF-REPAIR (%d,%d) t%d/h%d -> t%d/h%d L%d",
                x, y, c->IsoTileTypeIndex, c->Height, tile, height, c->Level);
        c->IsoTileTypeIndex = tile;
        c->Height           = static_cast<uint8_t>(height);
    };

    const int tWallCap  = shoreTileIndex_ + 33;   // C34  1x1 inner corner, z4
    const int tVStrip   = shoreTileIndex_ + 7;    // C8   1x2 vertical wall
    const int tFlatBase = shoreTileIndex_ + 11;  // C12..C14 flat caps, z0

    // ---- A. damaged multi-cell pieces (outcomes are contiguous per piece) ---
    size_t i = 0;
    while (i < cliffStamps_.size())
    {
        const CliffStampOutcome& first = cliffStamps_[i];
        size_t j = i;
        int blockedMask = 0;
        while (j < cliffStamps_.size()
               && cliffStamps_[j].ox == first.ox
               && cliffStamps_[j].oy == first.oy
               && cliffStamps_[j].slot == first.slot)
        {
            if (!cliffStamps_[j].ok)
                blockedMask |= 1 << cliffStamps_[j].h;
            ++j;
        }

        if (blockedMask != 0)
        {
            const int slot = first.slot;

            // A1: 2x2 wall whose east column (h1/h3) is the only blocked part.
            //     Surviving west column h0 (z4 origin) + h2 (z0 south) becomes
            //     the 1x2 vertical C8 strip.
            const bool eastColumnBlocked =
                (blockedMask == ((1 << 1) | (1 << 3)));
            bool haveH0 = false, haveH2 = false;
            for (size_t k = i; k < j; ++k)
            {
                if (!cliffStamps_[k].ok)
                    continue;
                if (cliffStamps_[k].h == 0) haveH0 = true;
                if (cliffStamps_[k].h == 2) haveH2 = true;
            }

            if (slot >= 4 && slot <= 7 && eastColumnBlocked && haveH0 && haveH2)
            {
                MapCell* top = CellAt(first.ox, first.oy);
                MapCell* foot = CellAt(first.ox,
                                       static_cast<int16_t>(first.oy + 1));
                if (stillHoldsPiece(top, slot) && top->Level >= 7
                    && stillHoldsPiece(foot, slot) && foot->Level <= 5)
                {
                    writeTile(first.ox, first.oy, tVStrip, 0);
                    writeTile(first.ox, first.oy + 1, tVStrip, 1);
                }
            }
            else
            {
                // A2: take the surviving cells apart into 1x1 pieces.
                for (size_t k = i; k < j; ++k)
                {
                    const CliffStampOutcome& o = cliffStamps_[k];
                    if (!o.ok)
                        continue;
                    MapCell* c = CellAt(o.x, o.y);
                    if (!stillHoldsPiece(c, slot))
                        continue;
                    if (o.z >= 4)
                    {
                        writeTile(o.x, o.y, tWallCap, 0);
                    }
                    else
                    {
                        const int pick = (static_cast<int>(o.ox)
                                        + static_cast<int>(o.oy) + o.h) % 3;
                        writeTile(o.x, o.y, tFlatBase + pick, 0);
                    }
                }
            }
        }

        i = j;
    }

    cliffStamps_.clear();
}

// ---------------------------------------------------------------------------
// CliffCellsInX - Array.Items[tile]->CellsInX for a CliffSet tile.
//
// sub_5A17F0 (the cliff correction pass, MapGenMakingSub.cpp) needs the
// family's column count to invert the Height a stamped cliff cell carries:
// Height encodes col + row * CellsInX, so col = Height % CellsInX and
// row = Height / CellsInX recover the cell's offset inside the tile. The port
// tabulates the cliff geometry as kCliffFootprints (above), whose `w` is
// exactly that column count, so this is a range-checked table read. A tile
// outside the 40-wide CliffSet family answers 1, which keeps the caller's
// modulo and division defined.
// ---------------------------------------------------------------------------
int RandomMapGenerator::CliffCellsInX(int tile) const
{
    const int slot = tile - shoreTileIndex_;
    if (slot < 0 || slot >= 40)
        return 1;
    return kCliffFootprints[slot].w;
}

// ---------------------------------------------------------------------------
// IsRampArtCell / IsCliffWallRiding / CliffPieceHitsRamp / TrimCliffPieceForRamp
// - port-only pre-stamp ramp-collision handling for PlaceCliffPiece.
//
// A multi-cell ramp face (3x4 / 4x3 SlopeSetPiece or a builder strip) meets a
// cliff wall in two visually different ways, and they must be told apart:
//
//  * Ramp-top seam: the L8 wall cell is met at its own level (L7/L8) by a ramp
//    cell - the ramp's top row / shoulder. The wall facade then drops onto the
//    ramp face naturally; that facade is REQUIRED art (it fills the notch
//    between a cliff plateau and the ramp top). Removing it leaves a white
//    triangle.
//
//  * Ramp-body ride: every ramp neighbour sits >= 2 levels below the wall and
//    no ramp cell meets it at the top level - the wall was dropped into the
//    ramp corridor and its facade rides down the ramp face, cutting the ramp.
//
// Only the second kind is rejected, and even then the surviving footprint
// cells are re-stamped as smaller CliffSet pieces (C8 vertical strip, or C34a
// / C12-14 single cells) instead of being flattened to t0 - same prescription
// as RepairCliffPieces: a partially covered piece becomes a smaller piece, not
// bare ground.
//
// Eight map neighbours are used (not four orthogonal): the ramp's L8 top meets
// a wall orthogonally while its L4..L6 body sits on the wall's diagonal side,
// and in the iso projection the facade occupies that diagonal space.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsRampArtCell(const MapCell* n) const
{
    if (n == nullptr)
        return false;
    if (n->SlopeIndex != 0)
        return true;
    const int t = n->IsoTileTypeIndex;
    return (cliffRampsIndex_ >= 0
            && t >= cliffRampsIndex_ && t < cliffRampsIndex_ + 20)
        || (rampBaseIndex_ >= 0
            && t >= rampBaseIndex_ && t < rampBaseIndex_ + 15)
        || (slopeSetPiecesIndex_ >= 0
            && t >= slopeSetPiecesIndex_ && t < slopeSetPiecesIndex_ + 10);
}

// A "big ramp piece" is the multi-cell SlopeSetPieces family (3x4 / 4x3 faces).
// Its art spans several cells and can reach the diagonal screen space of a wall,
// so a diagonal neighbour of this family can still ride. The single-cell
// families (CliffRamps, RampBase) only paint their own cell and never overlap a
// diagonally placed wall - they are checked on the four orthogonal sides only.
bool RandomMapGenerator::IsBigRampPiece(const MapCell* n) const
{
    if (n == nullptr)
        return false;
    const int t = n->IsoTileTypeIndex;
    return slopeSetPiecesIndex_ >= 0
        && t >= slopeSetPiecesIndex_ && t < slopeSetPiecesIndex_ + 10;
}

bool RandomMapGenerator::IsCliffWallRiding(int wx, int wy, int wallLevel)
{
    // The wall is built on the low cell; its drawn face (the facade the player
    // sees) points AWAY from the high ground, i.e. toward the low side. Only a
    // ramp sitting in front of that facade can be covered by it. A ramp on the
    // side or behind the facade never overlaps it.
    //
    // Step 1: sum the direction vectors of every high neighbour (Level >=
    // wallLevel) to get the "high ground direction". The facade points the
    // opposite way.
    // Step 2: pick the 8-direction index closest to the facade direction.
    // Step 3: only check that direction and its two diagonal neighbours for
    // ramp art (orthogonal: any ramp; diagonal: big multi-cell ramp pieces).
    static const int kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    // index: 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW

    int highX = 0, highY = 0;
    for (int d = 0; d < 8; ++d)
    {
        const int16_t nx = static_cast<int16_t>(wx + kDirX[d]);
        const int16_t ny = static_cast<int16_t>(wy + kDirY[d]);
        if (!CellExists(nx, ny))
            continue;
        const MapCell* n = CellAt(nx, ny);
        if (n->Level >= wallLevel)
        {
            highX += kDirX[d];
            highY += kDirY[d];
        }
    }
    const int faceX = -highX;
    const int faceY = -highY;

    int bestDir = -1;
    int bestDot = -1000;
    for (int d = 0; d < 8; ++d)
    {
        const int dot = kDirX[d] * faceX + kDirY[d] * faceY;
        if (dot > bestDot)
        {
            bestDot = dot;
            bestDir = d;
        }
    }
    if (bestDir < 0)
        return false;

    bool lowRamp = false;
    bool topRamp = false;
    // Only the cell directly in front of the facade can be ridden by the wall.
    // Ramps on the diagonal neighbours beside the facade lead somewhere else
    // and must not count.
    {
        const int d = bestDir;
        const int16_t nx = static_cast<int16_t>(wx + kDirX[d]);
        const int16_t ny = static_cast<int16_t>(wy + kDirY[d]);
        if (CellExists(nx, ny))
        {
            const MapCell* n = CellAt(nx, ny);
            const bool isRamp = (d % 2 == 0) ? IsRampArtCell(n)
                                             : IsBigRampPiece(n);
            if (isRamp)
            {
                if (n->Level <= wallLevel - 2)
                {
                    lowRamp = true;
                    // 斜对角的大坡格只有在坡面真的伸到墙立面下方时才算
                    // 骑坡。CliffSet 的立面只沿正交方向，所以当斜向两个正交
                    // 邻居里有一个是非坡、且高度不高于该坡格的地面时，它就是
                    // 与坡同层接平的墙脚行（坡顶在 (x,y-1)、坡底在 (x+1,y-1)、
                    // 墙脚在 (x+1,y) 这种共线衔接），坡只是悬崖线的延续而不
                    // 是被墙切断。少了这道关，全场墙片放完后的高度场会让高台
                    // 合成方向偏向斜角，把本来完好的墙片在拆除阶段误拆掉。
                    if ((d & 1) != 0)
                    {
                        const MapCell* gx = CellAt(
                            static_cast<int16_t>(wx + kDirX[d]),
                            static_cast<int16_t>(wy));
                        const MapCell* gy = CellAt(
                            static_cast<int16_t>(wx),
                            static_cast<int16_t>(wy + kDirY[d]));
                        const int rl = n->Level;
                        if ((gx != nullptr && !IsRampArtCell(gx)
                             && gx->Level <= rl)
                            || (gy != nullptr && !IsRampArtCell(gy)
                                && gy->Level <= rl))
                        {
                            lowRamp = false;
                        }
                    }
                }
                if (n->Level >= wallLevel - 1)
                    topRamp = true;
            }
        }
    }
    DiagLog("RIDING (%d,%d) L%d faceDir=%d low=%d top=%d",
            wx, wy, wallLevel, bestDir, lowRamp ? 1 : 0, topRamp ? 1 : 0);
    return lowRamp && !topRamp;
}

bool RandomMapGenerator::CliffPieceHitsRamp(const MapCell* anchor, int slot)
{
    if (anchor == nullptr || slot < 1 || slot > 40)
        return false;

    const IsoFootprint& fp = kCliffFootprints[slot - 1];
    const int ax = static_cast<int16_t>(anchor->MapCoords & 0xFFFF);
    const int ay = static_cast<int16_t>((uint32_t)anchor->MapCoords >> 16);
    const int baseLevel = anchor->Level;

    int zIndex = 0;
    for (int row = 0; row < fp.h; ++row)
    {
        for (int col = 0; col < fp.w; ++col)
        {
            if (((fp.mask >> (row * fp.w + col)) & 1ULL) == 0)
                continue;
            const int z = fp.z[zIndex];
            ++zIndex;
            const int x = ax + col;
            const int y = ay + row;
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const MapCell* c = CellAt(static_cast<int16_t>(x),
                                      static_cast<int16_t>(y));
            if (z >= 4)
            {
                // Wall cell: riding the ramp facade is the original conflict.
                if (IsCliffWallRiding(x, y, baseLevel + z))
                    return true;
            }
            else
            {
                // Foot cell: if it lands on ramp art the wall has no base and
                // floats over the ramp body. Treat as a hit so the trim pass
                // can drop the piece instead of stamping a wall without feet.
                if (IsRampArtCell(c))
                    return true;
            }
        }
    }
    return false;
}

// Re-stamp the footprint cells of a rejected piece as smaller CliffSet art:
//   - a 2x2 wall (slots 4-7) whose EAST column is the riding side collapses to
//     the C8 1x2 vertical strip (t56) on its surviving west column;
//   - every other surviving wall cell becomes the C34 1x1 corner (t82), and
//     every surviving foot cell one of C12..C14 (t60..t62).
// Only placeholder cells are written, so ramp / plateau art is never touched.
int RandomMapGenerator::TrimCliffPieceForRamp(const MapCell* anchor, int slot)
{
    const IsoFootprint& fp = kCliffFootprints[slot - 1];
    const int ax = static_cast<int16_t>(anchor->MapCoords & 0xFFFF);
    const int ay = static_cast<int16_t>((uint32_t)anchor->MapCoords >> 16);
    const int baseLevel = anchor->Level;

    struct CellRef { int x, y, h, level; };
    std::vector<CellRef> walls;
    std::vector<CellRef> feet;
    int zIndex = 0;
    for (int row = 0; row < fp.h; ++row)
    {
        for (int col = 0; col < fp.w; ++col)
        {
            if (((fp.mask >> (row * fp.w + col)) & 1ULL) == 0)
                continue;
            const int z = fp.z[zIndex++];
            const int x = ax + col;
            const int y = ay + row;
            if (!CellExists(static_cast<int16_t>(x), static_cast<int16_t>(y)))
                continue;
            const MapCell* c = CellAt(static_cast<int16_t>(x),
                                      static_cast<int16_t>(y));
            if (!IsPlaceholderTile(c))
                continue;                               // ramp / art: keep it
            const int newLevel = baseLevel + z;
            if (z >= 4 && IsCliffWallRiding(x, y, newLevel))
                continue;                               // the rejected side
            (z >= 4 ? walls : feet)
                .push_back(CellRef{ x, y, row * fp.w + col, newLevel });
        }
    }

    int written = 0;
    // Nothing has been stamped yet, so like PlaceIsoTile the wall cells must
    // also be RAISED to their piece level (anchor L4 + z4 = L8); feet keep the
    // anchor level.
    const auto write = [&](int x, int y, int tile, int height, int level)
    {
        MapCell* c = CellAt(static_cast<int16_t>(x), static_cast<int16_t>(y));
        if (c == nullptr || !IsPlaceholderTile(c))
            return;
        DiagLog("CLIFF-TRIM slot=%d (%d,%d) -> t%d/h%d L%d",
                slot, x, y, tile, height, level);
        c->IsoTileTypeIndex = tile;
        c->Height = static_cast<uint8_t>(height);
        c->Level = level;
        ++written;
    };

    const int tVStrip   = shoreTileIndex_ + 7;    // C8
    const int tWallCap  = shoreTileIndex_ + 33;   // C34
    const int tFlatBase = shoreTileIndex_ + 11;   // C12..C14

    // C8: surviving west column h0 (wall z4) + h2 (foot z0) of a 2x2 wall.
    if (slot >= 4 && slot <= 7 && baseLevel + 4 >= 7 && baseLevel <= 5)
    {
        const auto has = [&](int h) {
            for (const CellRef& r : walls) if (r.h == h) return true;
            for (const CellRef& r : feet)  if (r.h == h) return true;
            return false;
        };
        if (has(0) && has(2))
        {
            write(ax, ay, tVStrip, 0, baseLevel + 4);
            write(ax, ay + 1, tVStrip, 1, baseLevel);
            return written;
        }
    }

    for (const CellRef& r : walls)
        write(r.x, r.y, tWallCap, 0, r.level);
    for (const CellRef& r : feet)
        write(r.x, r.y, tFlatBase + (ax + ay + r.h) % 3, 0, r.level);
    return written;
}

// ---------------------------------------------------------------------------
// BuildWaterRing - sub_5A0700 (0x5A0700) - ring 0 of a water body.
//
// Sweeps every work cell (a slot counts only when its MapCoords in data[0] is
// non-zero) and keeps those that carry `genCode`. For each kept cell it scans
// the 8 neighbours inside the diamond and appends the cell once per neighbour
// that does NOT carry the same code - so ring 0 is the body's boundary, with
// one entry per outside neighbour (vanilla never de-duplicates; the consumer
// tolerates the repeats because an already-marked neighbour is skipped).
//
// The sweep order is the work array's linear order, so the ring is
// deterministic. The per-cell code lookup re-derives the index from the cell's
// own MapCoords (0x5a0788 / 0x5a07b5), exactly as vanilla does.
// ---------------------------------------------------------------------------
std::vector<CellStruct> RandomMapGenerator::BuildWaterRing(int genCode) const
{
    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    std::vector<CellStruct> ring;
    const int side = size_.workSide;

    for (int idx = 0; idx < side * side; ++idx)
    {
        const int coords = workCells_[idx].data[0];      // MapCoords
        if (coords == 0)
            continue;                                    // 0x5a0788
        const int16_t x = static_cast<int16_t>(coords & 0xFFFF);
        const int16_t y = static_cast<int16_t>((uint32_t)coords >> 16);
        if (workCells_[x + side * y].data[14] != genCode)
            continue;                                    // 0x5a07b5

        for (int dir = 0; dir < 8; ++dir)
        {
            const int16_t nx = static_cast<int16_t>(x + kDirX[dir]);
            const int16_t ny = static_cast<int16_t>(y + kDirY[dir]);
            if (!CellExists(nx, ny))
                continue;                                // 0x5a07f1
            if (workCells_[nx + side * ny].data[14] == genCode)
                continue;                                // still ours
            ring.push_back(CellStruct{ x, y });          // 0x5a0891
        }
    }
    return ring;
}

// ---------------------------------------------------------------------------
// IsWaterFamilyTile - sub_4865D0 (0x4865D0) - water / shore tile test.
//
// Reads IsoTileTypeIndex (CellClass +0x38, seen as this[14] on a _DWORD*) and
// returns true for, in order:
//   [shorePieces_,     shorePieces_ + 42)   game global nIdx_4 @0xABAD28
//   [waterTileIndex_,  waterTileIndex_ + 14) game global nIdx  @0xAA0738
//   [nIdx_0, +4)  [nIdx_1, +4)  [nIdx_2, +4)  [nIdx_3, +4)
// All six bases are filled by the theater tile INI at runtime; the project
// carries them as members (values still 0, see the header).
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsWaterFamilyTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;

    const int tile = cell->IsoTileTypeIndex;
    if (tile >= shorePieces_ && tile < shorePieces_ + 42)
        return true;
    if (tile >= waterTileIndex_ && tile < waterTileIndex_ + 14)
        return true;
    for (int i = 0; i < 4; ++i)
    {
        if (tile >= waterFamily4Base_[i] && tile < waterFamily4Base_[i] + 4)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// IsShoreTile - sub_4865B0 (0x4865B0) - the shore family test alone.
//
//   tile in [shorePieces_, shorePieces_ + 42)      game global nIdx_4 @0xABAD28
//
// The narrower sibling of IsWaterFamilyTile (sub_4865D0), which is the same
// range plus the water set and the four waterfall families. Its only consumer
// in the port is the hill stage's protection pre-mark, sub_5A33F0.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsShoreTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;

    const int tile = cell->IsoTileTypeIndex;
    return (shorePieces_ >= 0 && tile >= shorePieces_ && tile < shorePieces_ + 42);
}

// ---------------------------------------------------------------------------
// IsGreenGroundTile - sub_4867B0 (0x4867B0) - green-ground family test.
//
//   tile == dword_AA0E18                    GreenTile: the base green ground
//                                           tile of the theater
//   or tile in [dword_AA0748, +16)          ClearToGreenLat: the clear-to-green
//                                           LAT transition tiles
//
// (0x4867b0 reads CellClass +0x38 into v1 and returns exactly that test.)
//
// Both globals hold the running tile count of the theater INI [General] keys
// of the same names - verified in IsometricTileTypeClass::ReadINI: the
// "GreenTile" ReadInteger result is stored into dword_AA0E18 at 0x545d54, the
// "ClearToGreenLat" one into dword_AA0748 at 0x545d9f. dword_AA0E18 is the
// global the IDB names IsoTileTypeIndex_1; it is NOT the CliffSet global -
// CliffSet lives at dword_AA1020 and is carried here as shoreTileIndex_.
//
// This is the Init-regions stage's second seeding / growth gate (sub_58CF90
// 0x58cfe9, sub_58C800 0x58c882, sub_58E740 0x58e8a9): a cell qualifies when
// it is water family (sub_4865D0) OR green ground.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsGreenGroundTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;

    const int tile = cell->IsoTileTypeIndex;
    return tile == greenTileIndex_                                   // 0x4867bd
        || (tile >= clearToGreenLatIndex_
            && tile < clearToGreenLatIndex_ + 16);
}

// ---------------------------------------------------------------------------
// The four pave-family tile tests of the lateral-corridor builder (sub_58F2C0)
// and its rectangle predicates.
//
//   sub_4866D0  0x4866D0  tile in [dword_ABBEC8, +15)  PavedRoads
//   sub_4866F0  0x4866F0  tile in [dword_ABBEC4, + 4)  PavedRoadEnds
//   sub_486650  0x486650  tile in [dword_AA10A4, +14)  MiscPaveTile
//   sub_486670  0x486670  tile in [dword_ABC2B0, +16)  PaveTile
//
// Each global is the running tile total of the theater INI [General] key of the
// same name (stored by IsometricTileTypeClass::ReadINI at 0x545eda / 0x545ee9 /
// 0x545d72 / 0x545d63); LoadTheaterTiles fills the four members.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::IsPavedRoadTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;
    const int tile = cell->IsoTileTypeIndex;
    return tile >= pavedRoadsIndex_ && tile < pavedRoadsIndex_ + 15;
}

bool RandomMapGenerator::IsPavedRoadEndTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;
    const int tile = cell->IsoTileTypeIndex;
    return tile >= pavedRoadEndsIndex_ && tile < pavedRoadEndsIndex_ + 4;
}

bool RandomMapGenerator::IsMiscPaveTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;
    const int tile = cell->IsoTileTypeIndex;
    return tile >= miscPaveTileIndex_ && tile < miscPaveTileIndex_ + 14;
}

bool RandomMapGenerator::IsPaveTile(const MapCell* cell) const
{
    if (cell == nullptr)
        return false;
    const int tile = cell->IsoTileTypeIndex;
    return tile >= paveTileIndex_ && tile < paveTileIndex_ + 16;
}

// ---------------------------------------------------------------------------
// ExpandWaterBody - sub_5A0160 (0x5A0160) - ring expansion / absorption.
//
// Vanilla (recovered in full):
//   ring = sub_5A0700(this, genCode);                 // 0x5a0171, ring 0
//   for (i = 0; i < mode; ++i)                        // 0x5a0181, one ring/step
//   {
//     next = new vector; next.capacity = 3 * ring.count;   // 0x5a01cc
//     for each cell in ring                              // 0x5a01f3
//       for dir in 0..7                                  // 0x5a01fd
//       {
//         n = cell + Neighbours[dir];                    // 16-bit adds
//         if (!in diamond || !inside rect) continue;     // 0x5a0285
//         code = work[n].data[14];
//         prev = flag && code == genCode - 1;            // 0x5a02b6
//         if ((code == 0 || prev) &&
//             (placeholder(cell) || (prev && waterFamily(cell))))
//         {
//             next.push(n);                              // 0x5a033b
//             work[n].data[14] = genCode;                // 0x5a0348
//             if (flag && placeholder(cell))
//                 cell->Level = level;                   // 0x5a036b
//         }
//         else if (code != genCode)
//             return 0;                                  // pollution 0x5a03fd
//       }
//     ring = next;
//   }
//   return 1;                                            // 0x5a03d2
//
// Notes: no RNG is consumed. mode is a ring count, not a style switch - the
// call sites use 1 / 2 / 6. The rect gates use the same diamond bounds as
// CellExists; the incoming cell is looked up with GetCellAt_MapCrd, which
// cannot miss because the diamond gate already passed.
// ---------------------------------------------------------------------------
bool RandomMapGenerator::ExpandWaterBody(int genCode, int mode, int centerX,
                                         int centerY, int rangeX, int rangeY,
                                         int flag, int level)
{
    static const int16_t kDirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int16_t kDirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    std::vector<CellStruct> ring = BuildWaterRing(genCode);

    for (int step = 0; step < mode; ++step)
    {
        std::vector<CellStruct> next;
        next.reserve(3 * ring.size());                   // 0x5a01cc hint

        for (size_t k = 0; k < ring.size(); ++k)
        {
            const CellStruct src = ring[k];
            for (int dir = 0; dir < 8; ++dir)
            {
                const int16_t nx = static_cast<int16_t>(src.X + kDirX[dir]);
                const int16_t ny = static_cast<int16_t>(src.Y + kDirY[dir]);

                if (!CellExists(nx, ny))
                    continue;                            // 0x5a0285
                if (nx < centerX || nx >= centerX + rangeX ||
                    ny < centerY || ny >= centerY + rangeY)
                    continue;                            // rect window

                WorkCell& work = workCells_[nx + size_.workSide * ny];
                const int  code = work.data[14];
                const bool previousGeneration = (flag != 0 && code == genCode - 1);
                MapCell*   cell = cellSlots_[512 * ny + nx];

                if ((code == 0 || previousGeneration) &&
                    (IsPlaceholderTile(cell) ||
                     (previousGeneration && IsWaterFamilyTile(cell))))
                {
                    next.push_back(CellStruct{ nx, ny });        // 0x5a033b
                    work.data[14] = genCode;                     // 0x5a0348
                    if (flag != 0 && IsPlaceholderTile(cell))
                        cell->Level = level;                     // 0x5a036b
                }
                else if (code != genCode)
                {
                    return false;                                // 0x5a03fd
                }
            }
        }
        ring.swap(next);
    }
    return true;                                                 // 0x5a03d2
}

// ---------------------------------------------------------------------------
// DecorateWaterTiles - sub_59C630 - water-detail pass over open water.
//
// The main flow calls this unconditionally right after the terrain dispatch
// (sub_598960 @ 0x598b14), so every land type runs it - the 3/4 special-terrain
// path (river / lake water) included. It walks the diamond in iterator order
// and, for every plain open-water cell (IsoTileTypeIndex == WaterSet base tile
// and Height == 0) whose three "forward" neighbours E / S / SE (Neighbours 2 /
// 4 / 3) are plain water too:
//
//   roll n10  = F2I64(rand * 10  * kUnitScale + 1.0)  reject while > 10  -> 1..10
//   if n10 == 1, or any of the three neighbours was not plain water:
//       roll n200 = F2I64(rand * 201 * kUnitScale)     reject while > 200 -> 0..200
//       IsoTileTypeIndex = n200 / 40 + WaterSet + 8    (variants WaterSet+8..+13)
//   else:
//       roll n242 = F2I64(rand * 242 * kUnitScale)     reject while > 241 -> 0..241
//       sub = n242 < 240 ? n242 / 40 : 247 - n242      -> 0..7
//       PlaceWaterDetailTile(WaterSet + sub, coords, work gen mark, -1)
//
// So each plain-water cell costs exactly one RNG draw when the neighbour test
// sends it straight to the variant branch, two otherwise. Water cells that are
// not plain (different tile, or Height != 0) are skipped without a draw.
//
// The three scale constants are the exact doubles 10 / 201 / 242 * kUnitScale
// (0x7ED9E8 / 0x7ED9D8 / 0x7ED9E0 - checked byte for byte). The rejection
// loops never iterate, since each draw tops out exactly at the limit.
// ---------------------------------------------------------------------------
void RandomMapGenerator::DecorateWaterTiles()
{
    CellIterator it;
    it.Reset(cellSlots_, size_.mapWidth);
    while (MapCell* cell = it.Next())
    {
        // Plain open water only: WaterSet base tile, flat (Height 0).
        if (cell->IsoTileTypeIndex != waterTileIndex_ || cell->Height != 0)
            continue;                                     // 0x59c657

        const MapCell* nE  = GetNeighbourCell(cell, 2);   // 0x59c674
        const MapCell* nS  = GetNeighbourCell(cell, 4);   // 0x59c67d
        const MapCell* nSE = GetNeighbourCell(cell, 3);   // 0x59c688

        const bool plain =
            nE->IsoTileTypeIndex  == waterTileIndex_ && nE->Height  == 0 &&
            nS->IsoTileTypeIndex  == waterTileIndex_ && nS->Height  == 0 &&
            nSE->IsoTileTypeIndex == waterTileIndex_ && nSE->Height == 0;

        bool variant = !plain;                            // -> LABEL_22
        if (plain)
        {
            int n10;
            do
            {
                n10 = F2I64((double)(uint32_t)rng_.Next() * 10.0 * kUnitScale
                            + 1.0);                       // 0x59c6f0
            }
            while (n10 > 10);
            variant = (n10 == 1);                         // 0x59c709
        }

        if (variant)
        {
            int n200;
            do
            {
                n200 = F2I64((double)(uint32_t)rng_.Next()
                             * 201.0 * kUnitScale);       // 0x59c7b2
            }
            while (n200 > 200);
            cell->IsoTileTypeIndex = n200 / 40 + waterTileIndex_ + 8;  // 0x59c7da
        }
        else
        {
            int n242;
            do
            {
                n242 = F2I64((double)(uint32_t)rng_.Next()
                             * 242.0 * kUnitScale);       // 0x59c725
            }
            while (n242 > 241);
            const int sub = (n242 < 240) ? (n242 / 40) : (247 - n242);

            const int packedCoords = cell->MapCoords;
            const int genCode =
                WorkAt(packedCoords & 0xFFFF,
                       (uint32_t)packedCoords >> 16).data[14];  // 0x59c786
            PlaceWaterDetailTile(waterTileIndex_ + sub, packedCoords, genCode,
                                 -1);                     // 0x59c795
        }
    }
}

// ---------------------------------------------------------------------------
// sub_5A6C10 (0x5a6c10 - 0x5a6d6e) - multi-cell tile stamping anchored at an
// explicit cell. The vanilla walks the tile object's own grid:
//
//   tile = Array.Items[tileIndex];
//   if (tile->GetImage() == nullptr) return;                 // 0x5a6c32
//   for (row = 0; row < tile->CellsInY; ++row)
//     for (col = 0; col < tile->CellsInX; ++col)
//     {
//         x = anchorX + col, y = anchorY + row;
//         if (!in diamond) continue;                         // 0x5a6c92 / 0x5a6cb4
//         frame = image[Height + 4] with Height = col + row * CellsInX;
//         if (!frame) continue;                              // 0x5a6ce2
//         cell->Height           = Height;                   // 0x5a6cea
//         cell->IsoTileTypeIndex = tile->ArrayIndex;         // 0x5a6cf4
//         if (level != -1)
//             cell->Level        = level + frame[+0x28] - 4; // 0x5a6d04
//         cell->SlopeIndex       = frame[+0x2A];             // 0x5a6d0d
//         work[+56]              = genCode;                  // 0x5a6d3a
//     }
//
// The port tabulates the geometry in place of the tile objects:
//   CellsInX / CellsInY   -> IsoFootprint::w / h
//   "frame != null" cells -> IsoFootprint::mask, bit (row * w + col)
//   frame[+0x28]          -> IsoFootprint::z, indexed by the OCCUPIED-cell
//                            counter (empty cells contribute no entry)
//   frame[+0x2A]          -> NOT tabulated. The store below writes 0, which is
//                            exact for the water-detail pieces this routine
//                            started with; a cliff piece whose art carries a
//                            slope would need the value measured first.
//
// Two callers stamp through here now, which is why the geometry can no longer
// be hard-coded to 2x2: DecorateWaterTiles (the eight water detail tiles,
// waterTileIndex_ + 0..7, all full 2x2 rectangles with Z = 0) and
// CorrectCliffTiles (CliffSet pieces - width 1 or 2, height up to 3).
void RandomMapGenerator::PlaceWaterDetailTile(int tileIndex, int packedCoords,
                                              int genCode, int level)
{
    // The eight water-detail pieces (waterTileIndex_ + 0..7) really are plain
    // 2x2 rectangles with Z = 0 on every cell - the original caller this routine
    // was written for (DecorateWaterTiles).
    static const IsoFootprint k2x2Footprint = { 2, 2, 0xFULL };

    // TileSet0025 is BOTH [General] SlopeSetPieces and CliffRamps
    // (temperatmd.ini: CliffRamps = 25, FileName = RAMP -> ramp01..10): the
    // multi-cell cliff-ramp pieces every RampBuilder1..7 stamps. Measured
    // straight out of the TMPs - grid, non-empty sub-cell mask and each
    // occupied cell's frame[+0x28] z (cell Level = level + z - 4; z runs
    // 0..4, so an L8 owner fills L4..L8, the whole four-level ramp).
    //
    // The previous table hard-coded all ten pieces as a full 2x2 square. That
    // is only close for ramp07/ramp10 (2x2 with one empty cell); the other
    // eight are 3x4 / 4x3 with 7 or 10 occupied cells. The wrong walk wrote
    // Height = col + row*2 against a 3/4-wide TMP, so the frames did not line
    // up, most stamped cells failed TileCellHasFrame and Recalc washed them to
    // 0xFFFF - the long white strip hanging off every cliff ramp.
    static const IsoFootprint kSlopeSetPieceFootprints[10] =
    {
        { 3, 4, 0xDFEULL, { 3, 3, 4, 3, 2, 0, 0, 1, 0, 0 } }, // ramp01 .##/###/###/.##
        { 3, 4, 0x7FBULL, { 3, 3, 2, 3, 4, 1, 0, 0, 0, 0 } }, // ramp02 ##./###/###/##.
        { 4, 3, 0x6FFULL, { 3, 2, 1, 0, 3, 3, 0, 0, 4, 0 } }, // ramp03 ####/####/.##.
        { 4, 3, 0xFF6ULL, { 4, 0, 3, 3, 0, 0, 3, 2, 1, 0 } }, // ramp04 .##./####/####
        { 3, 4, 0xDFEULL, { 0, 0, 0, 0, 1, 4, 3, 2, 3, 3 } }, // ramp05 .##/###/###/.##
        { 3, 4, 0x27BULL, { 0, 0, 1, 0, 0, 2, 3 } },          // ramp06 ##./###/#../#..
        { 2, 2, 0x7ULL,  { 3, 4, 3 } },                        // ramp07 ##/#.
        { 4, 3, 0xFF6ULL, { 0, 4, 0, 0, 3, 3, 0, 1, 2, 3 } }, // ramp08 .##./####/####
        { 4, 3, 0x23FULL, { 0, 1, 2, 3, 0, 0, 0 } },          // ramp09 ####/##../.#..
        { 2, 2, 0x7ULL,  { 3, 3, 4 } },                        // ramp10 ##/#.
    };

    const IsoFootprint* fp = nullptr;
    if (waterTileIndex_ >= 0 && tileIndex >= waterTileIndex_ &&
        tileIndex < waterTileIndex_ + 8)
        fp = &k2x2Footprint;
    else if (slopeSetPiecesIndex_ >= 0 && tileIndex >= slopeSetPiecesIndex_ &&
             tileIndex < slopeSetPiecesIndex_ + 10)
        fp = &kSlopeSetPieceFootprints[tileIndex - slopeSetPiecesIndex_];
    else
        fp = FindFamilyFootprint(tileIndex, shorePieces_, shoreTileIndex_,
                                 waterCliffsIndex_, destroyableCliffsIndex_,
                                 waterFamily4Base_);
    if (fp == nullptr)
        return;                                       // no image: 0x5a6c32

    const int anchorX = static_cast<int16_t>(packedCoords & 0xFFFF);
    const int anchorY = static_cast<int16_t>((uint32_t)packedCoords >> 16);
    int zIndex = 0;                                   // occupied-cell counter

    for (int row = 0; row < fp->h; ++row)             // CellsInY
    {
        for (int col = 0; col < fp->w; ++col)         // CellsInX
        {
            if (((fp->mask >> (row * fp->w + col)) & 1ULL) == 0)
                continue;                             // frame == null: 0x5a6ce2
            const int z = fp->z[zIndex];
            ++zIndex;

            const int16_t x = static_cast<int16_t>(anchorX + col);
            const int16_t y = static_cast<int16_t>(anchorY + row);
            if (!CellExists(x, y))                    // 0x5a6cb4
                continue;

            MapCell* cell = CellAt(x, y);
            cell->Height           = col + row * fp->w;   // 0x5a6cea
            cell->IsoTileTypeIndex = tileIndex;           // 0x5a6cf4
            if (level != -1)                              // 0x5a6cfa
                cell->Level = level + z - 4;              // + frame[+0x28]
            cell->SlopeIndex       = 0;                   // frame[+0x2A]: not tabulated
            WorkAt(x, y).data[14] = genCode;              // 0x5a6d3a
        }
    }
}

// ---------------------------------------------------------------------------
// LoadTheaterTiles - the [General] tile-range snapshots of
// IsometricTileTypeClass::ReadINI (0x545150).
//
// ReadINI walks the theater's tile sets keeping a running global tile index
// and snapshots that index into a family global whenever it reaches the value
// of the matching [General] key, e.g.
//     0x545d4b: if (section == <"CliffSet" key>)         dword_AA1020 = running;
//     0x545d52: if (section == <"GreenTile" key>)        dword_AA0E18 = running;
//     0x545d9d: if (section == <"ClearToGreenLat" key>)  dword_AA0748 = running;
//
// IMPORTANT: the [General] values are TileSet SECTION NUMBERS, not tile
// indices - CliffSet = 10 means "[TileSet0010]", and the global that ReadINI
// stores is the RUNNING TILE COUNT at the moment that section is reached:
//
//   ShorePieces = 12  -> [TileSet0012] 'Shore Pieces' -> 89
//   WaterSet    = 21  -> [TileSet0021] 'Water'        -> 314
//   CliffSet    = 10  -> [TileSet0010] 'Cliff Set'    -> 49   (40 wide)
//   GreenTile         -> the green ground set (dword_AA0E18, the IDB names it
//                        IsoTileTypeIndex_1)      (sub_4867B0's first branch)
//   ClearToGreenLat   -> the clear-to-green LAT set, 16 wide
//                        (sub_4867B0's second branch)
//   WaterCliffs = 15  -> [TileSet0015] 'Cliff/Water pieces' -> 148 (28 wide)
//   DestroyableCliffs = 56 -> [TileSet0056] 'Destroyable Cliffs' -> 572
//                       (snow: 61 -> 694; 2 wide)
//   WaterfallEast/West/South/North -> 'Waterfalls A..D' -> per theater
//   (temperate 49/51/30/50 -> 539/547/414/543, snow 35/37/30/36 ->
//   418/426/414/422)
//
// (Cross-check: the shore art measured out of ISOSNOW.MIX is a 42-entry block
// whose position corresponds to 89, matching the cumulative count.)
//
// So the globals are computed here the same way the game does it: walk the
// [TileSetNNNN] sections in numeric order, accumulate TilesInSet, and take the
// running total just before the named section.
//
// The theater INI sits next to the executable like RMGMD.INI. Vanilla builds
// the name as "%sMD.INI" from the theater name, so theater 0 -> TEMPERATMD.INI
// and theater 1 -> SNOWMD.INI. A missing key or file leaves -1.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void RandomMapGenerator::LoadTheaterTiles(int theater)
{
    // [FA2sp] s_theaterIniDir lets the host point at the game folder; empty
    // keeps the standalone behaviour (next to the executable). FA2 loads the
    // same file into CINI::Temperate / CINI::Snow, so the values match.
    wchar_t wdir[MAX_PATH];
    if (!s_theaterIniDir.empty())
    {
        wcsncpy_s(wdir, MAX_PATH, s_theaterIniDir.c_str(), _TRUNCATE);
    }
    else
    {
        GetModuleFileNameW(nullptr, wdir, MAX_PATH);
        wchar_t* slash = wcsrchr(wdir, L'\\');
        if (slash)
            slash[1] = L'\0';
    }

    wchar_t wpath[MAX_PATH];
    swprintf_s(wpath, L"%s%s", wdir,
               theater == 1 ? L"SNOWMD.INI" : L"TEMPERATMD.INI");

    char path[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);

    // Section numbers named by [General].
    const int shoreSection = (int)GetPrivateProfileIntA(
        "General", "ShorePieces", (UINT)-1, path);
    const int waterSection = (int)GetPrivateProfileIntA(
        "General", "WaterSet", (UINT)-1, path);
    const int cliffSection = (int)GetPrivateProfileIntA(
        "General", "CliffSet", (UINT)-1, path);
    const int greenTileSection = (int)GetPrivateProfileIntA(
        "General", "GreenTile", (UINT)-1, path);
    const int clearToGreenLatSection = (int)GetPrivateProfileIntA(
        "General", "ClearToGreenLat", (UINT)-1, path);
    const int eastSection = (int)GetPrivateProfileIntA(
        "General", "WaterfallEast", (UINT)-1, path);
    const int westSection = (int)GetPrivateProfileIntA(
        "General", "WaterfallWest", (UINT)-1, path);
    const int southSection = (int)GetPrivateProfileIntA(
        "General", "WaterfallSouth", (UINT)-1, path);
    const int northSection = (int)GetPrivateProfileIntA(
        "General", "WaterfallNorth", (UINT)-1, path);
    const int waterCliffsSection = (int)GetPrivateProfileIntA(
        "General", "WaterCliffs", (UINT)-1, path);
    const int destroyableCliffsSection = (int)GetPrivateProfileIntA(
        "General", "DestroyableCliffs", (UINT)-1, path);
    const int cliffRampsSection = (int)GetPrivateProfileIntA(
        "General", "CliffRamps", (UINT)-1, path);
    const int waterCavesSection = (int)GetPrivateProfileIntA(
        "General", "WaterCaves", (UINT)-1, path);
    const int bridgeSetSection = (int)GetPrivateProfileIntA(
        "General", "BridgeSet", (UINT)-1, path);
    const int woodBridgeSetSection = (int)GetPrivateProfileIntA(
        "General", "WoodBridgeSet", (UINT)-1, path);
    const int rampBaseSection = (int)GetPrivateProfileIntA(
        "General", "RampBase", (UINT)-1, path);
    const int slopeSetPiecesSection = (int)GetPrivateProfileIntA(
        "General", "SlopeSetPieces", (UINT)-1, path);

    // The rest of the tile-family keys. SetupLAT (sub_47CA80) is the consumer;
    // the ones it does not touch are still read so the table matches ReadINI's.
    const int rampSmoothSection = (int)GetPrivateProfileIntA(
        "General", "RampSmooth", (UINT)-1, path);
    const int mmRampBaseSection = (int)GetPrivateProfileIntA(
        "General", "MMRampBase", (UINT)-1, path);
    const int clearTileSection = (int)GetPrivateProfileIntA(
        "General", "ClearTile", (UINT)-1, path);
    const int roughTileSection = (int)GetPrivateProfileIntA(
        "General", "RoughTile", (UINT)-1, path);
    const int sandTileSection = (int)GetPrivateProfileIntA(
        "General", "SandTile", (UINT)-1, path);
    const int clearToRoughLatSection = (int)GetPrivateProfileIntA(
        "General", "ClearToRoughLat", (UINT)-1, path);
    const int clearToSandLatSection = (int)GetPrivateProfileIntA(
        "General", "ClearToSandLat", (UINT)-1, path);
    const int clearToPaveLatSection = (int)GetPrivateProfileIntA(
        "General", "ClearToPaveLat", (UINT)-1, path);
    const int heightBaseSection = (int)GetPrivateProfileIntA(
        "General", "HeightBase", (UINT)-1, path);
    const int blackTileSection = (int)GetPrivateProfileIntA(
        "General", "BlackTile", (UINT)-1, path);
    const int slopeSetPieces2Section = (int)GetPrivateProfileIntA(
        "General", "SlopeSetPieces2", (UINT)-1, path);
    const int monorailSlopesSection = (int)GetPrivateProfileIntA(
        "General", "MonorailSlopes", (UINT)-1, path);
    const int tunnelsSection = (int)GetPrivateProfileIntA(
        "General", "Tunnels", (UINT)-1, path);
    const int trackTunnelsSection = (int)GetPrivateProfileIntA(
        "General", "TrackTunnels", (UINT)-1, path);
    const int dirtTunnelsSection = (int)GetPrivateProfileIntA(
        "General", "DirtTunnels", (UINT)-1, path);
    const int dirtTrackTunnelsSection = (int)GetPrivateProfileIntA(
        "General", "DirtTrackTunnels", (UINT)-1, path);
    const int mediansSection = (int)GetPrivateProfileIntA(
        "General", "Medians", (UINT)-1, path);
    const int roughGroundSection = (int)GetPrivateProfileIntA(
        "General", "RoughGround", (UINT)-1, path);
    const int dirtRoadJunctionSection = (int)GetPrivateProfileIntA(
        "General", "DirtRoadJunction", (UINT)-1, path);
    const int dirtRoadCurveSection = (int)GetPrivateProfileIntA(
        "General", "DirtRoadCurve", (UINT)-1, path);
    const int dirtRoadStraightSection = (int)GetPrivateProfileIntA(
        "General", "DirtRoadStraight", (UINT)-1, path);
    const int pavedRoadSlopesSection = (int)GetPrivateProfileIntA(
        "General", "PavedRoadSlopes", (UINT)-1, path);
    const int dirtRoadSlopesSection = (int)GetPrivateProfileIntA(
        "General", "DirtRoadSlopes", (UINT)-1, path);
    const int rocksSection = (int)GetPrivateProfileIntA(
        "General", "Rocks", (UINT)-1, path);
    const int waterBridgeSection = (int)GetPrivateProfileIntA(
        "General", "WaterBridge", (UINT)-1, path);
    // The four pave families sub_4866D0 / sub_4866F0 / sub_486650 / sub_486670
    // test and sub_58F2C0 stamps with. ReadINI stores their running totals into
    // dword_ABBEC8 / dword_ABBEC4 / dword_AA10A4 / dword_ABC2B0 (0x545eda,
    // 0x545ee9, 0x545d72, 0x545d63).
    const int pavedRoadsSection = (int)GetPrivateProfileIntA(
        "General", "PavedRoads", (UINT)-1, path);
    const int pavedRoadEndsSection = (int)GetPrivateProfileIntA(
        "General", "PavedRoadEnds", (UINT)-1, path);
    const int miscPaveTileSection = (int)GetPrivateProfileIntA(
        "General", "MiscPaveTile", (UINT)-1, path);
    const int paveTileSection = (int)GetPrivateProfileIntA(
        "General", "PaveTile", (UINT)-1, path);

    // Collect every [TileSetNNNN] section with its TilesInSet.
    std::vector<std::pair<int, int> > sets;
    std::vector<char> sectionNames(64 * 1024, '\0');
    const DWORD listLen = GetPrivateProfileSectionNamesA(
        sectionNames.data(), (DWORD)sectionNames.size(), path);
    if (listLen > 0)
    {
        for (const char* s = sectionNames.data(); *s; s += std::strlen(s) + 1)
        {
            if (_strnicmp(s, "TileSet", 7) != 0)
                continue;
            const int section = std::atoi(s + 7);
            const int tiles = (int)GetPrivateProfileIntA(s, "TilesInSet", 0, path);
            sets.push_back(std::make_pair(section, tiles));
        }
    }
    std::sort(sets.begin(), sets.end());

    // ---- per-tile Morphable flag -----------------------------------------
    // The INI documents the key as "Can this tile set be modified using the
    // raise/lower ground function?" - exactly what the hill stage does, so it
    // only levels cells whose tile set is morphable. Absolute tile indices
    // follow the same section order the RunningTotal bases below use.
    //
    // The vanilla reads it with CCINIClass::ReadBool(ini, section, "Morphable",
    // FALSE):
    //   0x546124  xor  ebx, ebx          <- the default it pushes (0x54612d)
    //   0x54613f  call CCINIClass::ReadBool
    // and ReadBool (0x5295F0) decides on the uppercased FIRST character of the
    // value: '0' / 'F' / 'N' -> false, '1' / 'T' / 'Y' -> true, and ANY other
    // character - like a missing value - falls back to the default, false
    // (0x529787 switch, `default: return a4`). So the port must not accept
    // arbitrary numbers, and must accept a bare "y" / "t".
    {
        int total = 0;
        for (size_t i = 0; i < sets.size(); ++i)
            total += sets[i].second;

        tileMorphable_.assign(static_cast<size_t>(total), 0);

        int running = 0;
        for (size_t i = 0; i < sets.size(); ++i)
        {
            char section[32];
            sprintf_s(section, "TileSet%04d", sets[i].first);
            char value[16] = "";
            GetPrivateProfileStringA(section, "Morphable", "",
                                     value, sizeof(value), path);

            const char lead = (value[0] != '\0')
                            ? static_cast<char>(toupper(
                                  static_cast<unsigned char>(value[0])))
                            : '\0';
            if (lead == '1' || lead == 'T' || lead == 'Y')
            {
                for (int k = 0; k < sets[i].second; ++k)
                    tileMorphable_[running + k] = 1;
            }

            running += sets[i].second;
        }
    }

    // Running tile count in section order, up to (excluding) `section`.
    struct RunningTotal
    {
        static int Start(const std::vector<std::pair<int, int> >& v, int section)
        {
            if (section < 0)
                return -1;
            int cum = 0;
            for (size_t i = 0; i < v.size(); ++i)
            {
                if (v[i].first >= section)
                    break;
                cum += v[i].second;
            }
            return cum;
        }
    };

    shorePieces_            = RunningTotal::Start(sets, shoreSection);
    waterTileIndex_         = RunningTotal::Start(sets, waterSection);
    shoreTileIndex_         = RunningTotal::Start(sets, cliffSection);
    greenTileIndex_         = RunningTotal::Start(sets, greenTileSection);
    rampBaseIndex_          = RunningTotal::Start(sets, rampBaseSection);
    slopeSetPiecesIndex_    = RunningTotal::Start(sets, slopeSetPiecesSection);
    rampSmoothIndex_        = RunningTotal::Start(sets, rampSmoothSection);
    mmRampBaseIndex_        = RunningTotal::Start(sets, mmRampBaseSection);
    clearTileIndex_         = RunningTotal::Start(sets, clearTileSection);
    roughTileIndex_         = RunningTotal::Start(sets, roughTileSection);
    sandTileIndex_          = RunningTotal::Start(sets, sandTileSection);
    clearToRoughLatIndex_   = RunningTotal::Start(sets, clearToRoughLatSection);
    clearToSandLatIndex_    = RunningTotal::Start(sets, clearToSandLatSection);
    clearToPaveLatIndex_    = RunningTotal::Start(sets, clearToPaveLatSection);
    heightBaseIndex_        = RunningTotal::Start(sets, heightBaseSection);
    blackTileIndex_         = RunningTotal::Start(sets, blackTileSection);
    slopeSetPieces2Index_   = RunningTotal::Start(sets, slopeSetPieces2Section);
    monorailSlopesIndex_    = RunningTotal::Start(sets, monorailSlopesSection);
    tunnelsIndex_           = RunningTotal::Start(sets, tunnelsSection);
    trackTunnelsIndex_      = RunningTotal::Start(sets, trackTunnelsSection);
    dirtTunnelsIndex_       = RunningTotal::Start(sets, dirtTunnelsSection);
    dirtTrackTunnelsIndex_  = RunningTotal::Start(sets, dirtTrackTunnelsSection);
    mediansIndex_           = RunningTotal::Start(sets, mediansSection);
    roughGroundIndex_       = RunningTotal::Start(sets, roughGroundSection);
    dirtRoadJunctionIndex_  = RunningTotal::Start(sets, dirtRoadJunctionSection);
    dirtRoadCurveIndex_     = RunningTotal::Start(sets, dirtRoadCurveSection);
    dirtRoadStraightIndex_  = RunningTotal::Start(sets, dirtRoadStraightSection);
    pavedRoadSlopesIndex_   = RunningTotal::Start(sets, pavedRoadSlopesSection);
    dirtRoadSlopesIndex_    = RunningTotal::Start(sets, dirtRoadSlopesSection);
    rocksIndex_             = RunningTotal::Start(sets, rocksSection);
    waterBridgeIndex_       = RunningTotal::Start(sets, waterBridgeSection);
    pavedRoadsIndex_        = RunningTotal::Start(sets, pavedRoadsSection);
    pavedRoadEndsIndex_     = RunningTotal::Start(sets, pavedRoadEndsSection);
    miscPaveTileIndex_      = RunningTotal::Start(sets, miscPaveTileSection);
    paveTileIndex_          = RunningTotal::Start(sets, paveTileSection);
    clearToGreenLatIndex_   = RunningTotal::Start(sets, clearToGreenLatSection);
    waterCliffsIndex_       = RunningTotal::Start(sets, waterCliffsSection);
    destroyableCliffsIndex_ = RunningTotal::Start(sets, destroyableCliffsSection);
    cliffRampsIndex_        = RunningTotal::Start(sets, cliffRampsSection);
    waterCavesIndex_        = RunningTotal::Start(sets, waterCavesSection);
    bridgeSetIndex_         = RunningTotal::Start(sets, bridgeSetSection);
    woodBridgeSetIndex_     = RunningTotal::Start(sets, woodBridgeSetSection);
    waterFamily4Base_[0] = RunningTotal::Start(sets, eastSection);
    waterFamily4Base_[1] = RunningTotal::Start(sets, westSection);
    waterFamily4Base_[2] = RunningTotal::Start(sets, southSection);
    waterFamily4Base_[3] = RunningTotal::Start(sets, northSection);
}
