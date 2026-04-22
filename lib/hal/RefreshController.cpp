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
    case PARTIAL: // TODO: real partial-window API; for now fall through
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
  (void)r;  // reserved for future partial-window API
  // Partial today == FAST, doesn't count toward budget (same as Shrike design).
  submit(PARTIAL, turnOffScreen, /*imagePage=*/false);
}
