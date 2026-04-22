#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/ShrikeLogo240.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // The 240x240 Shrike plate is shifted upward so the wordmark and
  // status line can sit comfortably below it without crowding the
  // version footer.
  const int logoSize = 240;
  const int logoY = (pageHeight - logoSize) / 2 - 40;

  renderer.clearScreen();
  renderer.drawImage(ShrikeLogo240, (pageWidth - logoSize) / 2, logoY, logoSize, logoSize);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 110, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 140, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
