// Kyrylo: головний екран «Блокнот» (макет В5).
//
//   [wifi]          ^_^            76%
//   ┌──────┐  читаю зараз
//   │обкла-│  Граф Монте-Крісто
//   │динка │  Александр Дюма
//   │      │  глава XX · 23%
//   └──────┘  [=====------]
//     ▌закладка        котик із символів
//   нещодавні:
//   [book] [book] [book]   (з відсотками)
//   ───── прямий узор ─────
//   ~~ папка ~~ (бібліотека) ~~ налаштування ~~   (терновий узор)
//
// Керування: стрілки/бокові кнопки ходять по книжках і значках, «Вибрати» відкриває,
// «Назад» — продовжити поточну книжку, довге «Вибрати» на папці — передача по Wi‑Fi.

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "HomeActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "images/NotebookAssets.h"
#include "notebookFontIds.h"
#include "util/BookProgress.h"

namespace {

constexpr int SCREEN_W = 480;

// Розкладка (пікселі, портрет 480×800) — точно як на макеті В5.
constexpr int BOOK_X = 34, BOOK_Y = 82, BOOK_W = 196, BOOK_H = 280, BOOK_T = 12;
constexpr int TEXT_X = 254, TEXT_W = 205;
constexpr int RECENT_HEAD_Y = 418;
constexpr int SMALL_W = 124, SMALL_H = 178, SMALL_GAP = 24;
constexpr int SMALL_Y = RECENT_HEAD_Y + 40;
constexpr int ORN_Y = SMALL_Y + SMALL_H + 26;
constexpr int VINE_X = (SCREEN_W - NbVineW) / 2;
constexpr int VINE_CY = 752;
constexpr int VINE_Y = VINE_CY - NbVineIconY;

void drawMono(const GfxRenderer& r, const uint8_t* bits, const int x, const int y, const int w, const int h,
              const bool ink = true) {
  const int stride = (w + 7) / 8;
  for (int yy = 0; yy < h; ++yy) {
    const uint8_t* row = bits + yy * stride;
    for (int xx = 0; xx < w; ++xx) {
      if (!(row[xx >> 3] & (0x80 >> (xx & 7)))) r.drawPixel(x + xx, y + yy, ink);
    }
  }
}

void fillCircle(const GfxRenderer& r, const int cx, const int cy, const int rad, const bool state) {
  for (int dy = -rad; dy <= rad; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) ++dx;
    r.drawLine(cx - dx, cy + dy, cx + dx, cy + dy, state);
  }
}

void progressBar(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int pct) {
  r.drawRoundedRect(x, y, w, h, 2, h / 2, true);
  if (pct > 0) {
    const int fw = std::max(h, w * std::min(pct, 100) / 100);
    r.fillRoundedRect(x, y, fw, h, h / 2, Color::Black);
  }
}

bool drawThumb(const GfxRenderer& r, const std::string& path, const Rect slot) {
  if (path.empty()) return false;
  HalFile file;
  if (!Storage.openFileForRead("HOME", path, file)) return false;
  Bitmap bitmap(file);
  // Мініатюра малює лише чорні крапки — без білої підкладки крізь неї видно чорну «задню кришку».
  r.fillRect(slot.x, slot.y, slot.width, slot.height, false);
  bool ok = bitmap.parseHeaders() == BmpReaderError::Ok && BaseTheme::drawCoverThumbFill(r, bitmap, slot);
  file.close();
  return ok;
}

// Заглушка, якщо в книжки немає обкладинки: рамка й назва.
void drawPlainCover(const GfxRenderer& r, const RecentBook& book, const Rect slot, const int fontId) {
  r.fillRect(slot.x, slot.y, slot.width, slot.height, false);
  r.drawRect(slot.x + 6, slot.y + 6, slot.width - 12, slot.height - 12, true);
  const auto lines = r.wrappedText(fontId, book.title.c_str(), slot.width - 24, 4);
  const int lh = r.getLineHeight(fontId);
  int y = slot.y + slot.height / 3 - static_cast<int>(lines.size()) * lh / 2;
  for (const auto& l : lines) {
    const int w = r.getTextWidth(fontId, l.c_str());
    r.drawText(fontId, slot.x + (slot.width - w) / 2, y, l.c_str(), true);
    y += lh;
  }
}

std::string thumbPathFor(const std::string& bookPath, const int height, const bool generate) {
  if (FsHelpers::hasReflowableBookExtension(bookPath)) {
    auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
    if (!epub) return "";
    std::string p = epub->getThumbBmpPath(height);
    if (Storage.exists(p.c_str())) return p;
    if (!generate) return "";
    if (epub->load(false, true) && epub->generateThumbBmp(height) && Storage.exists(p.c_str())) return p;
    return "";
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    auto xtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
    if (!xtc) return "";
    std::string p = xtc->getThumbBmpPath(height);
    if (Storage.exists(p.c_str())) return p;
    if (!generate) return "";
    if (xtc->load() && xtc->generateThumbBmp(height) && Storage.exists(p.c_str())) return p;
    return "";
  }
  return "";
}

// «XX. Цвинтар замку Іф» -> «глава XX»; інакше — сама назва.
std::string chapterLabelFor(const std::string& bookPath) {
  if (!FsHelpers::hasReflowableBookExtension(bookPath)) return "";
  auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!epub || !epub->load(false, true)) return "";
  HalFile file;
  if (!Storage.openFileForRead("HOME", epub->getCachePath() + "/progress.bin", file)) return "";
  uint8_t data[2]{};
  const int n = file.read(data, 2);
  file.close();
  if (n != 2) return "";
  const int spine = data[0] | (data[1] << 8);
  const int toc = epub->getTocIndexForSpineIndex(spine);
  if (toc < 0) return "";
  std::string title = epub->getTocItem(toc).title;
  const size_t dot = title.find(". ");
  if (dot != std::string::npos && dot > 0 && dot <= 8) {
    bool roman = true;
    for (size_t i = 0; i < dot; ++i) {
      const char c = title[i];
      if (!(strchr("IVXLCM", c) || (c >= '0' && c <= '9'))) roman = false;
    }
    if (roman) return "глава " + title.substr(0, dot);
  }
  return title;
}

}  // namespace

void HomeActivity::nbLoadInfo() {
  const int n = static_cast<int>(recentBooks.size());
  nbProgress.assign(n, -1);
  nbThumbBig.assign(n, "");
  nbThumbSmall.assign(n, "");
  nbChapterLabel.clear();

  // Обкладинок ще немає -> генеруємо (один раз на книжку, далі лежать у кеші).
  bool needGenerate = false;
  for (int i = 0; i < n; ++i) {
    const int h = i == 0 ? BOOK_H : SMALL_H;
    if (thumbPathFor(recentBooks[i].path, h, false).empty()) needGenerate = true;
  }
  Rect popup{};
  if (needGenerate) {
    popup = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    GUI.fillPopupProgress(renderer, popup, 5);
  }
  for (int i = 0; i < n; ++i) {
    const auto& path = recentBooks[i].path;
    if (i == 0) {
      nbThumbBig[i] = thumbPathFor(path, BOOK_H, true);
      nbChapterLabel = chapterLabelFor(path);
    } else {
      nbThumbSmall[i] = thumbPathFor(path, SMALL_H, true);
    }
    nbProgress[i] = loadBookProgress(path);
    if (needGenerate) GUI.fillPopupProgress(renderer, popup, 10 + 90 * (i + 1) / std::max(1, n));
  }
  nbInfoLoaded = true;
}

void HomeActivity::nbRender() {
  if (!nbInfoLoaded) nbLoadInfo();
  const GfxRenderer& r = renderer;
  const int n = static_cast<int>(recentBooks.size());
  renderer.clearScreen();

  // ── верхній рядок: Wi‑Fi (коли підключено), ^_^, заряд у відсотках ──
  if (WiFi.status() == WL_CONNECTED) {
    for (const int rad : {11, 6}) {
      r.drawArc(rad, 30, 27, -1, -1, 2, true);
      r.drawArc(rad, 30, 27, 1, -1, 2, true);
    }
    r.fillRect(29, 26, 3, 3, true);
  }
  drawMono(r, NbFace, (SCREEN_W - NbFaceW) / 2, 14, NbFaceW, NbFaceH);
  {
    char batt[8];
    snprintf(batt, sizeof(batt), "%u%%", static_cast<unsigned>(powerManager.getBatteryPercentage()));
    const int w = r.getTextWidth(NB_DIGITS_FONT_ID, batt);
    r.drawText(NB_DIGITS_FONT_ID, SCREEN_W - 22 - w, 12, batt, true);
  }

  // ── поточна книжка: обкладинка + корінець + сторінки + закладка ──
  if (n > 0) {
    const RecentBook& book = recentBooks[0];
    const int x = BOOK_X, y = BOOK_Y, w = BOOK_W, h = BOOK_H, t = BOOK_T;
    r.fillRect(x + 4, y + t, w + t - 6, h, true);  // задня кришка
    {
      const int bx[] = {x + 8, x + w, x + w + t - 4, x + 12};
      const int by[] = {y + h, y + h, y + h + t - 4, y + h + t - 4};
      r.fillPolygon(bx, by, 4, false);
      const int sx[] = {x + w, x + w + t - 4, x + w + t - 4, x + w};
      const int sy[] = {y + 6, y + t + 2, y + h + t - 4, y + h};
      r.fillPolygon(sx, sy, 4, false);
    }
    for (int k = 3; k < t - 4; k += 3) {  // лінії сторінок
      r.drawLine(x + 12 + k / 3, y + h + k, x + w + k - 2, y + h + k, true);
      r.drawLine(x + w + k, y + 6 + k, x + w + k, y + h + k - 2, true);
    }
    {  // закладка-стрічка
      const int rx = x + 40;
      const int px[] = {rx, rx + 14, rx + 14, rx + 7, rx};
      const int py[] = {y + h - 4, y + h - 4, y + h + 56, y + h + 47, y + h + 56};
      r.fillPolygon(px, py, 5, true);
    }
    const Rect slot{x, y, w, h};
    if (!drawThumb(r, nbThumbBig[0], slot)) drawPlainCover(r, book, slot, NB_HAND_16_FONT_ID);
    r.drawRect(x, y, w, h, 2, true);
    r.fillRect(x, y, 10, h, true);            // корінець
    r.drawLine(x + 12, y + 2, x + 12, y + h - 2, 2, false);  // згин
    if (selectorIndex == 0) r.drawRect(x - 7, y - 7, w + t + 12, h + t + 12, 3, true);

    // ── текст праворуч ──
    r.drawText(NB_HAND_12_FONT_ID, TEXT_X, 82, "читаю зараз", true);
    r.drawLine(TEXT_X, 114, TEXT_X + 150, 112, 2, true);
    int ty = 124;
    const int lh16 = r.getLineHeight(NB_HAND_16_FONT_ID);
    for (const auto& line : r.wrappedText(NB_HAND_16_FONT_ID, book.title.c_str(), TEXT_W, 3)) {
      r.drawText(NB_HAND_16_FONT_ID, TEXT_X, ty, line.c_str(), true);
      ty += lh16 - 4;
    }
    const int lh12 = r.getLineHeight(NB_HAND_12_FONT_ID);
    ty += 4;
    if (!book.author.empty()) {
      for (const auto& line : r.wrappedText(NB_HAND_12_FONT_ID, book.author.c_str(), TEXT_W, 2)) {
        r.drawText(NB_HAND_12_FONT_ID, TEXT_X, ty, line.c_str(), true);
        ty += lh12;
      }
    }
    ty += 8;
    std::string prog = nbChapterLabel;
    if (nbProgress[0] >= 0) {
      if (!prog.empty()) prog += " · ";
      prog += std::to_string(nbProgress[0]) + "%";
    }
    if (!prog.empty()) {
      const auto lines = r.wrappedText(NB_HAND_12_FONT_ID, prog.c_str(), TEXT_W, 2);
      for (const auto& line : lines) {
        r.drawText(NB_HAND_12_FONT_ID, TEXT_X, ty, line.c_str(), true);
        ty += lh12;
      }
    }
    progressBar(r, TEXT_X, ty + 6, 190, 10, std::max(0, nbProgress[0]));
  } else {
    r.drawText(NB_HAND_16_FONT_ID, TEXT_X, 140, "ще немає книжок", true);
  }

  // котик із символів
  drawMono(r, NbCat, TEXT_X + 70, 300, NbCatW, NbCatH);

  // ── нещодавні ──
  if (n > 1) {
    r.drawText(NB_HAND_12_FONT_ID, 34, RECENT_HEAD_Y, "нещодавні:", true);
    int x = (SCREEN_W - 3 * SMALL_W - 2 * SMALL_GAP) / 2;
    for (int i = 1; i < n && i < NB_MAX_BOOKS; ++i) {
      const Rect slot{x, SMALL_Y, SMALL_W, SMALL_H};
      r.fillRect(x + 4, SMALL_Y + 4, SMALL_W, SMALL_H, true);  // тінь
      if (!drawThumb(r, nbThumbSmall[i], slot)) drawPlainCover(r, recentBooks[i], slot, NB_HAND_12_FONT_ID);
      r.drawRect(x, SMALL_Y, SMALL_W, SMALL_H, 2, true);
      const int pct = nbProgress[i];
      if (pct >= 0) {
        char txt[8];
        snprintf(txt, sizeof(txt), "%d%%", pct);
        const int tw = r.getTextWidth(SMALL_FONT_ID, txt);
        const int bx = x + SMALL_W - tw - 16, byy = SMALL_Y + 6, bw = tw + 10, bh = 22;
        r.fillRoundedRect(bx, byy, bw, bh, 8, Color::White);
        r.drawRoundedRect(bx, byy, bw, bh, 2, 8, true);
        r.drawText(SMALL_FONT_ID, bx + 5, byy + (bh - r.getLineHeight(SMALL_FONT_ID)) / 2, txt, true);
      }
      progressBar(r, x, SMALL_Y + SMALL_H + 12, SMALL_W, 7, std::max(0, pct));
      if (selectorIndex == i) r.drawRect(x - 6, SMALL_Y - 6, SMALL_W + 16, SMALL_H + 30, 3, true);
      x += SMALL_W + SMALL_GAP;
    }
  }

  // ── прямий узор і терновий узор зі значками ──
  drawMono(r, NbOrnament, (SCREEN_W - NbOrnamentW) / 2, ORN_Y, NbOrnamentW, NbOrnamentH);
  drawMono(r, NbVine, VINE_X, VINE_Y, NbVineW, NbVineH);
  const uint8_t* icons[NB_ICON_COUNT] = {NbIconFolder, NbIconLibrary, NbIconGear};
  const int iconW[NB_ICON_COUNT] = {NbIconFolderW, NbIconLibraryW, NbIconGearW};
  const int iconH[NB_ICON_COUNT] = {NbIconFolderH, NbIconLibraryH, NbIconGearH};
  for (int k = 0; k < NB_ICON_COUNT; ++k) {
    const int cx = VINE_X + NbVineIconX[k];
    const bool sel = selectorIndex == n + k;
    if (sel) fillCircle(r, cx, VINE_CY, k == 1 ? 26 : 25, true);  // вибране — чорне коло, значок білий
    drawMono(r, icons[k], cx - iconW[k] / 2, VINE_CY - iconH[k] / 2, iconW[k], iconH[k], !sel);
  }

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                 : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}

void HomeActivity::nbActivate() {
  const int n = static_cast<int>(recentBooks.size());
  if (selectorIndex < n) {
    onRecentBookSelected(recentBooks[selectorIndex].path);
    return;
  }
  switch (selectorIndex - n) {
    case 0:
      onFileBrowserOpen();
      break;
    case 1:
      onLibraryOpen();
      break;
    case 2:
      onSettingsOpen();
      break;
    default:
      break;
  }
}

void HomeActivity::nbLoop() {
  const int count = getMenuItemCount();
  const int n = static_cast<int>(recentBooks.size());

  buttonNavigator.onNext([this, count] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
    requestUpdate();
  });

  // Довге «Вибрати» на папці -> передача книжок по Wi‑Fi.
  if (selectorIndex == n && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 700)) {
    onFileTransferOpen();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && n > 0) {
    onRecentBookSelected(recentBooks[0].path);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    nbActivate();
  }
}
