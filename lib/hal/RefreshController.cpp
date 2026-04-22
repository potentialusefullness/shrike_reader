#include <RefreshController.h>
#include <Logging.h>

/*
 * RefreshController implementation.
 *
 * Ported from Shrike's components/gfx/src/refresh.c (ghost-budget + mode
 * coalescing logic). The FreeRTOS queue and semaphore plumbing is dropped
 * here: CrossPoint is single-threaded Arduino, so submit() just resolves
 * the target mode and calls straight through to HalDisplay.
 *
 * Thread-safety: call from the Arduino main task only (same constraint as
 * HalDisplay itself).
 */

RefreshController refreshController;

RefreshController::RefreshController()
    : ghostBudget_(15),      // default matches CrossPointSettings REFRESH_15
      fastCount_(0),
      forceHalfNext_(false) {}

void RefreshController::setGhostBudget(uint16_t pages) {
  ghostBudget_ = pages;
  // If the new budget is already exceeded (user dropped from 30 → 5 after
  // 10 fast refreshes), schedule a HALF on the next submit to catch up
  // rather than leaving the counter stranded above the new ceiling.
  if (ghostBudget_ > 0 && fastCount_ >= ghostBudget_) {
    forceHalfNext_ = true;
  }
}

void RefreshController::requestHalfNext() { forceHalfNext_ = true; }

HalDisplay::RefreshMode RefreshController::toHal(Mode m) {
  switch (m) {
    case FULL:    return HalDisplay::FULL_REFRESH;
    case HALF:    return HalDisplay::HALF_REFRESH;
    case PARTIAL: // PARTIAL only reaches here via submit(); submitPartial() uses
                  // displayWindow() directly. Treat PARTIAL-via-submit as FAST.
    case FAST:
    default:      return HalDisplay::FAST_REFRESH;
  }
}

void RefreshController::submit(Mode mode, bool turnOffScreen, bool imagePage) {
  // Resolve target mode with coalescing rules (Shrike refresh.c mode_severity).
  Mode target = mode;

  // Rule 1: an explicit upgrade request from a previous submit() trumps
  // whatever the caller asked for now, as long as we aren't already doing
  // something stronger.
  if (forceHalfNext_ && target < HALF) {
    target = HALF;
  }

  // Rule 2: ghost-budget auto-escalation. When enough FAST refreshes have
  // accumulated, the next FAST is silently upgraded to HALF so particles
  // settle back to true B/W.
  const bool countsTowardBudget = (mode == FAST && !imagePage);
  if (countsTowardBudget && ghostBudget_ > 0 &&
      fastCount_ + 1 >= ghostBudget_ && target < HALF) {
    target = HALF;
  }

  // Push to the panel.
  const HalDisplay::RefreshMode halMode = toHal(target);
  display.displayBuffer(halMode, turnOffScreen);

  // Update counters AFTER the push (matches Shrike refresh.c ordering).
  if (target >= HALF) {
    // HALF and FULL both clear the ghosting, so reset.
    fastCount_ = 0;
    forceHalfNext_ = false;
  } else if (countsTowardBudget) {
    fastCount_++;
  }
  // PARTIAL, image-page FAST: no counter change (as designed).
}

void RefreshController::submitPartial(const Rect& r, bool turnOffScreen) {
  // Partial refresh bypasses the ghost budget entirely: it's typically driven
  // by tiny UI ticks (battery %, page counter) that would otherwise pollute
  // the budget and force a gratuitous HALF on the next page turn.
  //
  // Byte alignment: SSD1677 requires X and width to be multiples of 8.
  // Rather than reject unaligned rects (which just drops the refresh on the
  // floor inside the driver), snap outward to the nearest 8-pixel boundary.
  // The caller's intent — "refresh at least this band" — is preserved, at
  // the cost of refreshing up to 7 extra pixels on each horizontal edge.
  int16_t x = r.x;
  int16_t w = r.w;
  if (x < 0) { w += x; x = 0; }
  if (w <= 0 || r.h <= 0) return;

  const int16_t xAligned = x & ~int16_t{7};           // snap down
  const int16_t rightEdge = x + w;
  const int16_t rightAligned = (rightEdge + 7) & ~int16_t{7};  // snap up
  const uint16_t wAligned = static_cast<uint16_t>(rightAligned - xAligned);

  display.displayWindow(static_cast<uint16_t>(xAligned),
                        static_cast<uint16_t>(r.y < 0 ? 0 : r.y),
                        wAligned,
                        static_cast<uint16_t>(r.h),
                        turnOffScreen);
  // Note: fastCount_ intentionally unchanged. Partial updates don't meaningfully
  // contribute to full-screen ghosting because they replay the same particles
  // under a fresh waveform each call.
}
