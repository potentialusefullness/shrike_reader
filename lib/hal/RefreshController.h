#pragma once
#include <Arduino.h>
#include <HalDisplay.h>

/*
 * RefreshController — ported from Shrike's components/gfx/refresh.c.
 *
 * Purpose
 * =======
 * Centralises EPD refresh-mode policy. Before this existed, every caller
 * that displayed a buffer had to track its own ghost-budget countdown
 * (`pagesUntilFullRefresh` in ReaderUtils, ad-hoc logic in the image-page
 * path in EpubReaderActivity, separate counter in XtcReader, etc.). That
 * bookkeeping is now the controller's job, and the call sites just say
 * "refresh please" without caring whether it'll be FAST or HALF today.
 *
 * Design (from Shrike/gfx/refresh.c, adapted for single-threaded Arduino):
 *
 *   - Tracks fastCount against a ghost budget. When the budget is reached
 *     the NEXT submit() is escalated to HALF_REFRESH and the counter resets.
 *
 *   - submit() is synchronous — Arduino/single-core, no task queue. But the
 *     same coalescing idea applies: if you submit FAST after having already
 *     escalated to HALF in the same frame (e.g. a caller that refreshed the
 *     status bar then immediately refreshed the full page), the stronger
 *     mode wins and the weaker one is a no-op.
 *
 *   - The ghost budget is sourced from SETTINGS.getRefreshFrequency(). Set
 *     it once on startup (setGhostBudget) and re-sync when the user changes
 *     the setting.
 *
 *   - FULL_REFRESH always runs, resets the counter, and never coalesces
 *     downward.
 *
 *   - PARTIAL mode is declared in the API for future use (page-counter tick,
 *     status bar updates); the current HalDisplay doesn't expose a partial
 *     path so today it falls through to FAST with a TODO. Once the eink
 *     driver grows a partial-window API this becomes a real pass-through.
 *
 * What this replaces
 * ==================
 *   - ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh)
 *     → refreshController.submit(FAST_REFRESH);
 *   - Per-activity `int pagesUntilFullRefresh` members → gone.
 *   - EpubReaderActivity imagePageWithAA bespoke double-FAST → still does
 *     its own thing but marks images as "image page" so they don't count
 *     toward the budget (matches the comment in line 781 of the original).
 *
 * Back-compat shim
 * ================
 * ReaderUtils::displayWithRefreshCycle is retained as a deprecated inline
 * wrapper that forwards to submit(FAST_REFRESH) and ignores the counter
 * argument. Existing call sites keep compiling while we migrate them.
 */
class RefreshController {
 public:
  enum Mode {
    PARTIAL = 1,  // smallest: rect update (future — falls through to FAST today)
    FAST    = 2,  // default: fast_refresh, counts toward ghost budget
    HALF    = 3,  // ghost-clearing: resets budget, mid-speed
    FULL    = 4,  // full waveform: resets budget, slowest
  };

  struct Rect {
    int16_t x, y, w, h;
  };

  RefreshController();

  /**
   * @brief Set the ghost budget — HALF_REFRESH is injected after this many
   *        consecutive FAST_REFRESH calls. 0 disables auto-escalation.
   *
   * Typical values match SETTINGS.getRefreshFrequency(): 1, 5, 10, 15, 30.
   */
  void setGhostBudget(uint16_t pages);

  /**
   * @brief Submit a refresh request.
   *
   * - FAST: increments fastCount. If fastCount >= ghostBudget, escalates to
   *         HALF and resets the counter.
   * - HALF / FULL: always runs, resets the counter.
   * - PARTIAL: today falls through to FAST, doesn't count toward budget.
   *
   * @param mode         requested mode
   * @param turnOffScreen pass-through to HalDisplay::displayBuffer
   * @param imagePage    if true, don't count this refresh against the ghost
   *                     budget. Used by the reader's pablohc double-FAST
   *                     path for images, which manages its own ghosting.
   */
  void submit(Mode mode, bool turnOffScreen = false, bool imagePage = false);

  /** Reserved for future partial-window support. Falls through to FAST today. */
  void submitPartial(const Rect& r, bool turnOffScreen = false);

  /** Force the next submit() to HALF regardless of budget. Used when a
   *  caller knows it painted a disruptive update (e.g. opening a book). */
  void requestHalfNext();

  /** Current state — mostly for diagnostics / tests. */
  uint16_t getFastCount() const { return fastCount_; }
  uint16_t getGhostBudget() const { return ghostBudget_; }

 private:
  uint16_t ghostBudget_;
  uint16_t fastCount_;
  bool     forceHalfNext_;

  static HalDisplay::RefreshMode toHal(Mode m);
};

extern RefreshController refreshController;
