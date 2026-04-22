#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "hyphenation/Hyphenator.h"

namespace {

// Shrike-derived Knuth-Plass demerit constants.
// Badness uses the cubic-ratio formulation from the Pixelpaper/Shrike reference renderer:
//   ratio   = (pageWidth - lineWidth) / pageWidth
//   badness = ratio^3 * 100
// Demerits for an interior line = (1 + badness)^2 + LINE_PENALTY + per-item hyphen_penalty.
// The last line may be loose (demerits = 0). These constants match
// shrike/components/gfx/src/line_break.c so both renderers produce identical paragraphs.
constexpr float KP_INFINITY_PENALTY = 10000.0f;
constexpr float KP_LINE_PENALTY = 50.0f;
constexpr float KP_HYPHEN_PENALTY = 50.0f;

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Knuth-Plass badness (Shrike/Pixelpaper formulation): ratio^3 * 100.
// Overfull lines return KP_INFINITY_PENALTY so they are never selected.
float kpBadness(const int lineWidth, const int pageWidth) {
  if (pageWidth <= 0) return KP_INFINITY_PENALTY;
  if (lineWidth > pageWidth) return KP_INFINITY_PENALTY;
  if (lineWidth == pageWidth) return 0.0f;
  const float ratio = static_cast<float>(pageWidth - lineWidth) / static_cast<float>(pageWidth);
  return ratio * ratio * ratio * 100.0f;
}

// Demerits for a candidate line, matching Shrike's shrike_kp_break().
// The last line of a paragraph is permitted to be loose (returns 0).
float kpDemerits(const float badness, const bool isLastLine, const bool hyphenatedBreak) {
  if (isLastLine) return 0.0f;
  if (badness >= KP_INFINITY_PENALTY) return KP_INFINITY_PENALTY;
  const float base = (1.0f + badness) * (1.0f + badness) + KP_LINE_PENALTY;
  return hyphenatedBreak ? base + KP_HYPHEN_PENALTY : base;
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
  wordHyphenated.push_back(false);
  hyphenExtraW.push_back(0);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer, fontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordHyphenated.erase(wordHyphenated.begin(), wordHyphenated.begin() + consumed);
    hyphenExtraW.erase(hyphenExtraW.begin(), hyphenExtraW.begin() + consumed);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec) {
  if (words.empty()) {
    return {};
  }

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // Knuth-Plass DP (right-to-left): dp[i] = minimum total demerits for the suffix
  // starting at word i; ans[i] = index of the last word on the optimal first line.
  //
  // Demerits (interior line): (1 + badness)^2 + LINE_PENALTY + optional HYPHEN_PENALTY
  //   badness = (ratio^3) * 100 where ratio = (pageWidth - lineWidth) / pageWidth.
  // Last line: demerits = 0 (permitted to be loose — matches TeX and Shrike).
  //
  // This replaces the prior quadratic-remaining-space cost. The KP formulation penalises
  // loose lines much more aggressively (cubic in the ratio), discourages short interior
  // lines via LINE_PENALTY, and penalises unnecessary mid-word hyphenated breaks.
  // Matches shrike/components/gfx/src/line_break.c so Shrike and CrossPoint produce
  // identical paragraph breaks given identical word widths.
  std::vector<float> dp(totalWordCount, 0.0f);
  std::vector<size_t> ans(totalWordCount);

  // Base Case — the suffix containing only the last word is the last line: zero demerits.
  dp[totalWordCount - 1] = 0.0f;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = static_cast<int>(totalWordCount) - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = KP_INFINITY_PENALTY * 1000.0f;
    bool foundAny = false;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && !continuesVec[j]) {
        gap =
            renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      }
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group),
      // UNLESS j is a split prefix (wordHyphenated[j] == true). In that case continues=true on
      // j+1 means "attach for same-line rendering if we DON'T break here", not "no-break group",
      // and breaking here is exactly the intended use of the speculative split.
      const bool nextIsContinuation = j + 1 < totalWordCount && continuesVec[j + 1];
      const bool jIsSplitPrefix = j < wordHyphenated.size() && wordHyphenated[j];
      if (nextIsContinuation && !jIsSplitPrefix) {
        continue;
      }

      const bool isLastLine = j == totalWordCount - 1;
      const bool hyphenatedBreak = !isLastLine && j < wordHyphenated.size() && wordHyphenated[j];
      // If the line ends on a deferred-hyphen prefix, the rendered hyphen widens the line.
      // Use (currlen + hyphenExtraW[j]) for badness so fitness decisions match what renders.
      // Overfull detection already happened above (currlen > effectivePageWidth); if the hyphen
      // tips us over, badness will be KP_INFINITY_PENALTY and this candidate is discarded.
      const int effectiveLineWidth = hyphenatedBreak && j < hyphenExtraW.size()
                                         ? currlen + static_cast<int>(hyphenExtraW[j])
                                         : currlen;
      const float badness = kpBadness(effectiveLineWidth, effectivePageWidth);
      const float lineDemerits = kpDemerits(badness, isLastLine, hyphenatedBreak);
      const float total = isLastLine ? lineDemerits : lineDemerits + dp[j + 1];

      if (total < dp[i]) {
        dp[i] = total;
        ans[i] = j;  // j is the index of the last word in this optimal line
        foundAny = true;
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (!foundAny) {
      ans[i] = i;  // Just this word on its own line
      // Inherit demerits from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0.0f;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent() {
  if (extraParagraphSpacing || words.empty()) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback EmSpace
    // The actual indent positioning is handled in extractLine()
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    // No CSS text-indent defined - use EmSpace fallback for visual indent
    words.front().insert(0, "\xe2\x80\x83");
  }
}

// Hyphenation-enabled layout path.
//
// Architecture: pre-materialize speculative mid-word splits for every word with legal
// hyphenation points, then delegate line-breaking to the Knuth-Plass DP in computeLineBreaks.
//
// The pre-pass uses hyphenateWordAtIndex in DEFERRED-HYPHEN mode: the visible '-' is not inserted
// into the word string, and the remainder's wordContinues flag is set to true so that if the KP
// DP decides not to break at the split, the prefix + remainder render as the single original
// word (with kerning, no space, no stray hyphen). If the DP does break at the split, the prefix
// renders its '-' at extractLine time and the remainder leads the next line.
//
// This gives KP-optimal line breaking for the hyphenated path: the DP considers breaks both at
// word boundaries AND at legal mid-word points, scoring each with the same badness / demerits /
// HYPHEN_PENALTY formulas used for the non-hyphenation path. Matches the TeX approach of
// representing hyphenation as "discretionary break" items.
//
// Oversized words: still handled by the eager-mode oversized-word prepass in computeLineBreaks.
// Any word whose natural width exceeds the page width after deferred splits are applied gets
// split again with the visible '-' baked in (eager mode) — prefixes of oversized words have no
// same-line-remainder option anyway, so eager mode is correct there.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  // Pre-pass: speculatively split every hyphenatable word at its widest legal breakpoint within
  // the page width. Iterate by index since the word vector grows on each split; freshly inserted
  // remainders are re-examined by the same loop so a word with multiple break points can be
  // split repeatedly (each split materialises one more candidate break for the DP).
  //
  // We pass availableWidth = pageWidth so hyphenateWordAtIndex picks the widest fitting prefix,
  // which is the most useful candidate for line-ending. Using a smaller budget would force
  // earlier-in-the-word splits that rarely help balance.
  //
  // allowFallbackBreaks is false here: speculative splits should only occur at legal
  // linguistic boundaries. The eager oversized-word pass inside computeLineBreaks still enables
  // fallback breaks for words that literally cannot fit on a line.
  //
  // Short words (< 6 bytes) are skipped as an optimisation — they rarely contain a usable break
  // point and every call to Hyphenator::breakOffsets is non-trivial.
  constexpr size_t MIN_HYPHENATABLE_BYTES = 6;
  for (size_t i = 0; i < words.size(); ++i) {
    // Skip words that are already the remainder of a previous deferred split (continues=true
    // AND wordHyphenated[i-1]=true, meaning the prior word is our prefix). Re-splitting the
    // remainder is fine in principle but adds cost without much benefit.
    if (words[i].size() < MIN_HYPHENATABLE_BYTES) continue;
    if (wordHyphenated.size() > i && wordHyphenated[i]) continue;  // already a prefix

    // Deferred-hyphen split: inserts a prefix/remainder pair at position i / i+1 without
    // committing to a visible hyphen. If the word has no legal break point, returns false and
    // leaves the vectors untouched.
    hyphenateWordAtIndex(i, pageWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/false,
                         /*deferHyphen=*/true);
    // Note: i advances normally next iteration — if a split occurred, the next iteration visits
    // the remainder (now at i+1), and the loop naturally considers splitting it too.
  }

  // Delegate line-breaking to the KP DP. It handles indent, continuation groups, oversized-word
  // fallback (via the same eager-mode path), and hyphen-penalty scoring.
  return computeLineBreaks(renderer, fontId, pageWidth, wordWidths, continuesVec);
}

// Splits words[wordIndex] into prefix + remainder at a legal hyphenation point.
//
// Eager-hyphen mode (deferHyphen=false, default): the visible '-' is inserted into the prefix
// string immediately; wordWidths[prefix] reflects the width including that '-'. The remainder's
// wordContinues flag is left false — caller is expected to break the line between them. This is
// the non-hyphenation path's oversized-word fallback: the split is irrevocable.
//
// Deferred-hyphen mode (deferHyphen=true): the '-' is NOT inserted. wordWidths[prefix] is the
// natural width of the prefix letters alone; the hyphen glyph width is cached in
// hyphenExtraW[prefix] so the DP and extractLine can add it only when the prefix actually ends a
// line. The remainder's wordContinues flag is set to true so that if the KP DP decides NOT to
// break at this split (prefix and remainder end up on the same line), the two halves render as a
// single concatenated word with no space between them (kerning only). If the DP DOES break here,
// the remainder starts the next line; wordContinues=true is ignored at line starts, so this is
// correct either way.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks, const bool deferHyphen) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;           // width used for the "does it fit" decision (includes hyphen in eager mode)
  int chosenNaturalWidth = -1;    // width of prefix letters alone (no hyphen) — used as stored wordWidths[prefix]
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const std::string prefixStr = word.substr(0, offset);
    const int fitWidth = measureWordWidth(renderer, fontId, prefixStr, style, needsHyphen);
    if (fitWidth > availableWidth || fitWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = fitWidth;
    chosenNaturalWidth = needsHyphen ? measureWordWidth(renderer, fontId, prefixStr, style, /*appendHyphen=*/false)
                                     : fitWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint.
  // Eager mode: append the '-' to the prefix string now so rendering matches without extra bookkeeping.
  // Deferred mode: leave the prefix bare; extractLine will append '-' when the prefix ends a line.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen && !deferHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);

  // Continuation flag for the remainder.
  //
  // Eager mode (classic non-hyphenation oversized-word fallback): remainder gets continues=false.
  // The prefix is guaranteed to be the last item on its line; the remainder starts the next line.
  // Example: "200&#xA0;Quadratkilometer" tokens after split:
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (prefix keeps original flag — still attached to no-break group)
  //   [3] "kilometer"   continues=false  (remainder: starts fresh on next line)
  //
  // Deferred mode (speculative split for KP DP): remainder gets continues=true. If the KP DP
  // keeps prefix and remainder on the same line (because splitting wasn't optimal), they render
  // as the single original word with only cross-boundary kerning between them — no inserted space.
  // If the DP breaks between them, continues=true is ignored because it's at a line start.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, deferHyphen);

  // Track hyphenation state for the DP (HYPHEN_PENALTY when line ends on this prefix) and for
  // extractLine (appends '-' and adds hyphenExtraW to line width accounting).
  //
  // Invariant: words.size() has already been incremented by the insert above; wordHyphenated /
  // hyphenExtraW are still at pre-split size (= words.size() - 1). We first ensure they're filled
  // out to pre-split size (defensive — addWord always appends in sync, but an earlier branch may
  // have skipped them), update the prefix's flag in place, then insert the remainder's slot so
  // the final sizes match words.size() again.
  const size_t preSplitSize = words.size() - 1;
  if (wordHyphenated.size() < preSplitSize) {
    wordHyphenated.resize(preSplitSize, false);
  }
  if (hyphenExtraW.size() < preSplitSize) {
    hyphenExtraW.resize(preSplitSize, 0);
  }
  // wordHyphenated marks a word as a split-prefix break point — true for BOTH eager and deferred
  // splits, and regardless of whether the break inserts a new '-' or uses an existing one.
  // It drives two DP/render decisions:
  //  1. DP allows ending a line at this word even if the next word is continues=true (this is
  //     the whole point of speculative splits; the no-break-group rule doesn't apply here).
  //  2. DP adds HYPHEN_PENALTY when ending a line here — mid-word breaks are typographically
  //     inferior to word-boundary breaks, even when no new hyphen glyph is drawn.
  // Whether a '-' is actually rendered is a separate question, governed by hyphenExtraW > 0.
  wordHyphenated[wordIndex] = true;
  wordHyphenated.insert(wordHyphenated.begin() + wordIndex + 1, false);  // remainder is never itself a split prefix

  // Hyphen extra width: how many pixels the '-' glyph adds at the end of this prefix.
  // Eager mode: 0 (the '-' is already baked into both the word string AND wordWidths[prefix]).
  // Deferred mode: fitWidth - chosenNaturalWidth (positive when a hyphen must be drawn).
  const uint16_t hyphenGlyphWidth =
      deferHyphen && chosenNeedsHyphen ? static_cast<uint16_t>(chosenWidth - chosenNaturalWidth) : 0;
  hyphenExtraW[wordIndex] = hyphenGlyphWidth;
  hyphenExtraW.insert(hyphenExtraW.begin() + wordIndex + 1, 0);  // remainder has no pending hyphen

  // Update cached widths to reflect the new prefix/remainder pairing.
  // Eager mode stores chosenWidth (hyphen-inclusive) so existing render/line-width math is unchanged.
  // Deferred mode stores chosenNaturalWidth (no hyphen); callers must add hyphenExtraW[prefix] when
  // the prefix actually ends a line.
  wordWidths[wordIndex] = static_cast<uint16_t>(deferHyphen ? chosenNaturalWidth : chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine && blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  //
  // Hyphenation note: if the last word on this line is a deferred-hyphen split prefix
  // (wordHyphenated[lastIdx] == true), the rendered text appends a '-' glyph that was NOT
  // included in wordWidths[lastIdx]. Its width is cached in hyphenExtraW[lastIdx] and is
  // added to lineWordWidthSum below so justification / right-align / center-align math
  // reflects the actual pixel width the line will occupy.
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
      totalNaturalGaps +=
          renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]),
                                   firstCodepoint(words[lastBreakAt + wordIdx]), wordStyles[lastBreakAt + wordIdx - 1]);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps +=
          renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]),
                              firstCodepoint(words[lastBreakAt + wordIdx]), wordStyles[lastBreakAt + wordIdx - 1]);
    }
  }

  // Deferred-hyphen rendering: if the line ends on a split prefix whose hyphen glyph was NOT
  // baked into the word string, include its width and schedule a '-' append on the emitted
  // text. Eager-mode splits (hyphenExtraW == 0) already have their hyphen inside the word and
  // inside wordWidths, so they skip this path — avoiding a double hyphen.
  const size_t lastWordIdxGlobal = lineBreak == 0 ? 0 : lineBreak - 1;
  const bool lineEndsOnDeferredHyphen = lineWordCount > 0 && lastWordIdxGlobal < hyphenExtraW.size() &&
                                        hyphenExtraW[lastWordIdxGlobal] > 0;
  if (lineEndsOnDeferredHyphen) {
    lineWordWidthSum += hyphenExtraW[lastWordIdxGlobal];
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For justified text, compute per-gap extra to distribute remaining space evenly
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1)
                               ? spareSpace / static_cast<int>(actualGapCount)
                               : 0;

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineXPos.push_back(xpos);

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    if (nextIsContinuation) {
      int advance = wordWidths[lastBreakAt + wordIdx];
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      advance +=
          renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                              firstCodepoint(words[lastBreakAt + wordIdx + 1]), wordStyles[lastBreakAt + wordIdx]);
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        gap = renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                       firstCodepoint(words[lastBreakAt + wordIdx + 1]),
                                       wordStyles[lastBreakAt + wordIdx]);
      }
      if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
        gap += justifyExtra;
      }
      xpos += wordWidths[lastBreakAt + wordIdx] + gap;
    }
  }

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords(std::make_move_iterator(words.begin() + lastBreakAt),
                                     std::make_move_iterator(words.begin() + lineBreak));
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  // Deferred-hyphen: append '-' to the last word on the line. This materialises the hyphen that
  // the DP was already pricing via HYPHEN_PENALTY + hyphenExtraW so the rendered glyphs match the
  // width reservation. Done after soft-hyphen stripping so the inserted '-' isn't accidentally
  // removed.
  if (lineEndsOnDeferredHyphen && !lineWords.empty()) {
    lineWords.back().push_back('-');
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}
