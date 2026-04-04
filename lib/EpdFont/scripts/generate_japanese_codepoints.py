#!/usr/bin/env python3
"""
JIS X 0213 (2004) に含まれる全漢字およびその他日本語関連Unicodeコードポイントを生成するスクリプト。

Unicodeコンソーシアムの Unihan データベースから kJis0208 / kJis0213 フィールドを持つ
漢字を抽出し、ひらがな・カタカナ・記号類などの固定範囲と合わせて出力する。
"""

import io
import os
import sys
import zipfile
import urllib.request
from datetime import datetime

# --- 設定 ---
UNIHAN_ZIP_URL = "https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip"
UNIHAN_ZIP_PATH = "/tmp/Unihan.zip"
OUTPUT_PATH = "/Users/zrn_ns/projects/crosspoint-reader/lib/EpdFont/scripts/codepoints/japanese_jis0213.txt"

# --- 固定コードポイント範囲 ---
FIXED_RANGES = [
    (0x0020, 0x007E),  # ASCII printable
    (0x0080, 0x00FF),  # Latin-1 Supplement
    (0x2000, 0x206F),  # General Punctuation
    (0x3000, 0x303F),  # CJK Symbols and Punctuation
    (0x3040, 0x309F),  # Hiragana
    (0x30A0, 0x30FF),  # Katakana
    (0x31F0, 0x31FF),  # Katakana Phonetic Extensions
    (0xFF00, 0xFFEF),  # Halfwidth and Fullwidth Forms
]

SINGLE_CODEPOINTS = [
    0xFFFD,  # Replacement character
]


def download_unihan(zip_path: str) -> None:
    """Unihan.zip をダウンロードする（既存ファイルがあればスキップ）。"""
    if os.path.exists(zip_path):
        print(f"既存の {zip_path} を使用します。")
        return
    print(f"Unihan.zip をダウンロード中: {UNIHAN_ZIP_URL}")
    urllib.request.urlretrieve(UNIHAN_ZIP_URL, zip_path)
    print(f"ダウンロード完了: {zip_path}")


def extract_jis_codepoints(zip_path: str) -> set[int]:
    """Unihan.zip から kJis0208 / kJis0213 フィールドを持つコードポイントを抽出する。"""
    # kJis0 = JIS X 0208, kJis1 = JIS X 0212, kJIS0213 = JIS X 0213
    # kJis0 と kJIS0213 を対象とする（kJis1 は JIS X 0212 で 0213 のサブセットではないが、
    # 実質的に日本語で使われる漢字なので含める）
    target_fields = {"kJis0", "kJis1", "kJIS0213"}
    codepoints: set[int] = set()

    with zipfile.ZipFile(zip_path, "r") as zf:
        # Unihan_OtherMappings.txt を探す
        mapping_file = None
        for name in zf.namelist():
            if "Unihan_OtherMappings" in name:
                mapping_file = name
                break

        if mapping_file is None:
            print("エラー: Unihan_OtherMappings.txt が見つかりません。", file=sys.stderr)
            print(f"ZIP内のファイル一覧: {zf.namelist()}", file=sys.stderr)
            sys.exit(1)

        print(f"解析中: {mapping_file}")
        with zf.open(mapping_file) as f:
            for raw_line in io.TextIOWrapper(f, encoding="utf-8"):
                line = raw_line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) >= 2 and parts[1] in target_fields:
                    # U+XXXX 形式からコードポイントを取得
                    cp_str = parts[0]
                    if cp_str.startswith("U+"):
                        cp = int(cp_str[2:], 16)
                        codepoints.add(cp)

    return codepoints


def collect_fixed_codepoints() -> set[int]:
    """固定範囲のコードポイントを収集する。"""
    codepoints: set[int] = set()
    for start, end in FIXED_RANGES:
        for cp in range(start, end + 1):
            codepoints.add(cp)
    for cp in SINGLE_CODEPOINTS:
        codepoints.add(cp)
    return codepoints


def main() -> None:
    # 1. Unihan.zip をダウンロード
    download_unihan(UNIHAN_ZIP_PATH)

    # 2. JIS漢字コードポイントを抽出
    jis_codepoints = extract_jis_codepoints(UNIHAN_ZIP_PATH)
    print(f"JIS X 0208/0213 漢字: {len(jis_codepoints)} 文字")

    # 3. 固定範囲のコードポイントを収集
    fixed_codepoints = collect_fixed_codepoints()
    print(f"固定範囲: {len(fixed_codepoints)} 文字")

    # 4. 結合してソート
    all_codepoints = sorted(jis_codepoints | fixed_codepoints)
    total = len(all_codepoints)
    print(f"合計: {total} 文字")

    # 5. 出力
    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open(OUTPUT_PATH, "w", encoding="utf-8") as f:
        f.write(f"# JIS X 0213 (2004) コードポイントリスト\n")
        f.write(f"# 生成日時: {now}\n")
        f.write(f"# 文字数: {total}\n")
        f.write(f"# 含有範囲: ASCII, Latin-1 Supplement, General Punctuation,\n")
        f.write(f"#   CJK Symbols and Punctuation, Hiragana, Katakana,\n")
        f.write(f"#   Katakana Phonetic Extensions, Halfwidth and Fullwidth Forms,\n")
        f.write(f"#   Replacement Character (U+FFFD),\n")
        f.write(f"#   JIS X 0208/0212/0213 漢字 (Unihan kJis0/kJis1/kJIS0213)\n")
        f.write(f"#\n")
        for cp in all_codepoints:
            f.write(f"{cp:04X}\n")

    print(f"出力完了: {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
