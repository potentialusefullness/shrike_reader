#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
OPENDYSLEXIC_FONT_SIZES=(8 10 12 14)

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# Noto Sans + Noto Sans JP (kana) fallback. The JP face supplies Japanese
# kana glyphs when the Latin face doesn't cover them. Italic/BoldItalic styles
# of NotoSansJP don't exist upstream, so we fall back to the Regular/Bold JP
# faces for those styles (reader rarely emphasises kana anyway).
#
# The 32 MB NotoSansJP .ttf files are .gitignored. Fetch them once with:
#   mkdir -p ../builtinFonts/source/NotoSansJP
#   curl -L -o ../builtinFonts/source/NotoSansJP/NotoSansJP-Regular.ttf \
#     https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf
#   curl -L -o ../builtinFonts/source/NotoSansJP/NotoSansJP-Bold.ttf \
#     https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Bold.otf
# (OFL 1.1 licensed; renamed .otf -> .ttf locally — fontconvert.py handles both.)
for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    latin_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    case "$style" in
      Regular|Italic) jp_path="../builtinFonts/source/NotoSansJP/NotoSansJP-Regular.ttf" ;;
      Bold|BoldItalic) jp_path="../builtinFonts/source/NotoSansJP/NotoSansJP-Bold.ttf" ;;
    esac
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $latin_path $jp_path --2bit --compress --pnum --cjk-kana > $output_path
    echo "Generated $output_path"
  done
done

for size in ${OPENDYSLEXIC_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="opendyslexic_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/OpenDyslexic/OpenDyslexic-${style}.otf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
