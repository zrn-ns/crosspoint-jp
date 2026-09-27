#!/usr/bin/env -S deno run --allow-read --allow-write
// 組版まわりの確認用 EPUB を作る。
//
//   deno run -A scripts/generate_layout_test_epubs.ts
//   → test/epubs/ja_kinsoku.epub  禁則処理（行頭禁則・行末禁則・分離禁止）
//   → test/epubs/ja_lists.epub    リスト（<ol> の連番・入れ子・start / value）
//   → test/epubs/ja_inline.epub   <hr> / <sup> / <sub> / CJK 間の半角スペース
//   → test/epubs/ja_headings.epub h1〜h6 と本文の間のアキ
//   → test/epubs/ja_pre.epub      コードブロック（枠・行間・br・ページまたぎ）
//   → test/epubs/ja_aozora_ruby.epub 青空文庫の〔〕付きルビ（issue #154）。_vertical は同じ内容の縦書き
//
// ja_kinsoku.epub の各段落は約物を一定周期で含むので、1 行あたりの文字数が何文字でも
// どこかの行で「行頭に約物が来る」状態が必ず発生する。修正前後の比較用。

import { resolve } from "jsr:@std/path@1";

const repoRoot = resolve(new URL("..", import.meta.url).pathname);

function esc(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function paras(...list: string[]): string {
  return list.map((p) => `<p>${esc(p)}</p>`).join("\n");
}

// --- ja_kinsoku.epub --------------------------------------------------------

const kinsokuChapters: { title: string; body: string }[] = [
  {
    title: "一 行頭禁則（句読点）",
    body: paras(
      "吾輩は猫である、名前はまだ無い。".repeat(12),
      "どこで生れたか頓と見当がつかぬ、何でも薄暗いじめじめした所で、にゃあにゃあ泣いて居た事丈は記憶して居る。"
        .repeat(4),
    ),
  },
  {
    title: "二 行頭禁則（閉じ括弧）・行末禁則（開き括弧）",
    body: paras(
      "「そうですか」と彼は言った。（本当に？）と私は思った。『吾輩は猫である』を読む。［注一］を見よ。".repeat(4),
      "彼は「まさか」と呟き、私は（そんな）と返し、母は『やめて』と叫んだ。".repeat(5),
    ),
  },
  {
    title: "三 行頭禁則（小書き仮名・長音・中点）",
    body: paras(
      "しゃっくりがぴたりと止まった、ジューッと焼ける音、ぎゅっと握った手。".repeat(5),
      "山田・鈴木・佐藤の三名、コーヒー・紅茶・ジュースのいずれか、ニューヨーク・パリ・ロンドン。".repeat(4),
    ),
  },
  {
    title: "四 行頭禁則（繰返し記号・ハイフン類）",
    body: paras(
      "日々是好日、人々の暮らし、山々の頂、その他いろゝゝの事、ハヽヽと笑う。".repeat(5),
      "東京〜大阪の移動、三〇〇〜四〇〇円、ＡＢＣ〜ＸＹＺの範囲。".repeat(5),
    ),
  },
  {
    title: "五 分離禁止（リーダー・ダッシュ）",
    body: paras(
      "そして……彼は――去った。残されたのは沈黙……ただそれだけ――だった。".repeat(5),
      "あのとき‥‥私は何も言えなかった。いまでも――そう、いまでも……悔やんでいる。".repeat(4),
    ),
  },
  {
    title: "六 感嘆符・疑問符",
    body: paras(
      "なんだって！ 本当か？ そんな馬鹿な！ ありえない？ いや、確かだ！".repeat(6),
      "待て！　行くな？　止まれ！　答えろ？　聞こえるか！".repeat(6),
    ),
  },
];

// --- ja_lists.epub ----------------------------------------------------------

// 1 枚のスクリーンショットで主要ケースが揃うよう、代表例は 1 章目に集約する。
const listChapters: { title: string; body: string }[] = [
  {
    title: "一 基本",
    // 同じ文字が続く項目を先頭に置いてある。折り返した行の頭が本文 1 文字目に
    // 揃っているかを、字形の左サイドベアリングに惑わされずに画素で測るため。
    body: `<ol><li>${"銀".repeat(48)}</li></ol>
<ul><li>${"銀".repeat(48)}</li></ul>
<ol>
<li>電源ボタンを長押しして起動する。</li>
<li>折り返した行がぶら下げインデントになることもここで見る、という長めの項目。</li>
</ol>
<ul><li>りんご</li><li>みかん</li></ul>
<ol>
<li>外側の一つ目
  <ol><li>内側の一つ目</li><li>内側の二つ目</li></ol>
</li>
<li>外側の二つ目
  <ul><li>中黒の項目</li></ul>
</li>
<li>外側の三つ目</li>
</ol>`,
  },
  {
    title: "二 順序なしリストとの並び",
    body: `<p>順序なしは中黒のままであること。</p>
<ul><li>りんご</li><li>みかん</li><li>ぶどう</li></ul>
<p>順序付きは連番になること。</p>
<ol><li>春</li><li>夏</li><li>秋</li><li>冬</li></ol>`,
  },
  {
    title: "三 start 属性と value 属性",
    body: `<p>start="5" から始まる。</p>
<ol start="5"><li>五番目</li><li>六番目</li><li>七番目</li></ol>
<p>value="10" で番号が飛び、以降もそこから続く。</p>
<ol><li>一番目</li><li value="10">十番目</li><li>十一番目</li></ol>`,
  },
  {
    title: "四 入れ子（詳細）",
    body: `<ol>
<li>外側の一つ目
  <ol><li>内側の一つ目</li><li>内側の二つ目</li></ol>
</li>
<li>外側の二つ目
  <ul><li>中黒の項目</li><li>中黒の項目</li></ul>
</li>
<li>外側の三つ目（内側を抜けたあと番号が続くこと）</li>
</ol>`,
  },
  {
    title: "五 二桁以上の番号",
    body: `<ol start="8">${
      Array.from({ length: 8 }, (_, i) => `<li>項目その${8 + i}</li>`).join("")
    }</ol>`,
  },
];

// --- ja_inline.epub ---------------------------------------------------------

const inlineChapters: { title: string; body: string }[] = [
  {
    title: "一 hr と sup / sub",
    body: `<p>次の行に区切り線が入る。</p>
<hr/>
<p>水は H<sub>2</sub>O、二酸化炭素は CO<sub>2</sub>。面積は 5m<sup>2</sup>、体積は 3m<sup>3</sup>。</p>
<p>脚注の番号<sup>1</sup>は本文<sup>23</sup>と区別できること。</p>
<hr/>
<p>区切り線のあと。</p>`,
  },
  {
    title: "二 pre（空白と改行の保持）",
    body: `<p>次はコードブロック。字下げと改行が残ること。</p>
<pre><code>function greet(name) {
  if (!name) {
    return "hello";
  }
  return "hello, " + name;
}
</code></pre>
<p>この行は pre を抜けたあと。字下げと均等割りが戻ること。長さを稼ぐための文をここに書いておく。</p>
<pre>  +-----+
  | AA  |   &lt;- 桁は揃わないが位置関係は残る
  +-----+
	タブ 1 つ（空白 4 つぶん）
折り返しが必要なくらい長い行をここに置いておく。折り返しても内容が消えないこと。</pre>
<p>pre のあとの段落。</p>
<pre>
</pre>
<p>中身が空の pre の直後の段落。枠線が残っていないこと、段落のアキが普通にあること。</p>`,
  },
  {
    // 整形のために原文が折り返してある XHTML。日本語の EPUB では珍しくない。
    // 改行は CSS Text のセグメント改行で、和文どうしの間では取り除かれる（空白にしない）。
    title: "三 整形された XHTML の改行",
    body: `<p>${
      "吾輩は猫である。名前はまだ無い。どこで生れたか頓と見当がつかぬ。何でも薄暗いじめじめした所でにゃあにゃあ泣いていた事だけは記憶している。"
        .replace(/(.{16})/g, "$1\n")
    }</p>
<p>
  ${
      "同じ文を字下げ付きで折り返したもの。改行の前後に空白があっても、和文どうしなら字間は空かない。ここが空くと原文の折り返し位置がすべて見えてしまう。"
        .replace(/(.{16})/g, "$1\n  ")
    }
</p>
<p>${"Latin words wrapped\nin the source must\nstill keep their spaces."}</p>`,
  },
  {
    title: "四 CJK の間の半角スペース",
    body: `<p>第一章 序 という見出し語。空白が残ること。</p>
<p>山田 太郎、鈴木 花子、佐藤 次郎。</p>
<p>空白の無い普通の文はこれまでどおり字間が空かないこと。吾輩は猫である。</p>`,
  },
];

// --- ja_headings.epub -------------------------------------------------------

// 見出しと本文の間のアキを見るためのもの。各見出しの下に段落を 2 つ置いてあるので、
// 「見出し → 本文」のアキと「段落 → 段落」のアキを同じ画面で見比べられる。
const P1 = "見出しのすぐ下の段落。ここと見出しの距離を見る。行送りを広げた設定ほど差が出るので、設定の行間は普段使っている値のままで確認する。";
const P2 = "2 つ目の段落。ここと 1 つ目の段落の距離が、段落どうしのアキ。見出しの下のアキはこれより少し広いくらいが適当で、倍もあると離れて見える。";

const headingChapters: { title: string; body: string }[] = [
  {
    title: "一 h1 h2 h3",
    body: `<h1>大見出し h1</h1>
<p>${esc(P1)}</p>
<p>${esc(P2)}</p>
<h2>中見出し h2</h2>
<p>${esc(P1)}</p>
<p>${esc(P2)}</p>
<h3>小見出し h3</h3>
<p>${esc(P1)}</p>
<p>${esc(P2)}</p>`,
  },
  {
    title: "二 h4 h5 h6",
    body: `<h4>見出し h4</h4>
<p>${esc(P1)}</p>
<p>${esc(P2)}</p>
<h5>見出し h5</h5>
<p>${esc(P1)}</p>
<p>${esc(P2)}</p>
<h6>見出し h6</h6>
<p>${esc(P1)}</p>
<p>${esc(P2)}</p>`,
  },
  {
    title: "三 見出しが続く場合",
    body: `<h2>中見出し h2</h2>
<h3>すぐ下に小見出し h3</h3>
<p>${esc(P1)}</p>
<h3>段落のあとにまた小見出し h3</h3>
<p>${esc(P2)}</p>`,
  },
];

// --- ja_pre.epub ------------------------------------------------------------

// コードブロックの枠と行間を見るためのもの。CSS で pre に上下の padding/margin を
// 指定してある（pandoc が出す EPUB と同じ形）。この余白が行ごとに掛かると
// 1 行おきに空きが入り、左右の縦線が途切れる。
const preCss = `pre {
  border: 1px solid #ddd;
  padding: 0.8em;
  margin: 1em 0;
  line-height: 1.5;
}
code { font-family: monospace; }
`;

// pandoc の syntax highlight と同じ、1 行ずつ <span> で囲んだ形
function hl(lines: string[]): string {
  const inner = lines.map((l, i) =>
    `<span id="cb-${i + 1}"><a href="#cb-${i + 1}" aria-hidden="true" tabindex="-1"></a>${esc(l)}</span>`
  ).join("\n");
  return `<div class="sourceCode"><pre class="sourceCode"><code>${inner}</code></pre></div>`;
}

const preChapters: { title: string; body: string }[] = [
  {
    title: "一 ハイライト付き（1 行 = 1 span）",
    body: `<p>${esc("下の枠が途切れず、行と行の間が詰まっていること。")}</p>
${
      hl([
        "# 悪い例: ワークフローを要約している",
        "description: Use when executing plans - dispatches subagent",
        "            with code review between tasks",
        "",
        "# 良い例: 発動条件のみ",
        "description: Use when executing implementation plans",
      ])
    }
<p>${esc("コードの途中の空行は空行のまま残り、そこでも枠が途切れないこと。")}</p>`,
  },
  {
    title: "二 素朴な pre・1 行・空行で始まる",
    body: `<p>${esc("span で囲まない素朴な pre。")}</p>
<pre><code>int main() {
    return 0;
}</code></pre>
<p>${esc("1 行だけの pre。枠の上下が 1 行を挟んで閉じること。")}</p>
<pre><code>echo hello</code></pre>
<p>${esc("空行で始まる pre。枠の上辺が出て、1 行目が空行になること。")}</p>
<pre><code>

先頭が空行</code></pre>
<p>${esc("中身が空の pre。次の段落との間隔が詰まりすぎないこと。")}</p>
<pre><code></code></pre>
<p>${esc("空の pre の直後の段落。")}</p>`,
  },
  {
    title: "三 br で改行する pre",
    body: `<p>${esc("DOCX 変換などで出てくる、改行文字ではなく br で行を分ける pre。")}</p>
<pre><code>line1<br/>line2<br/>line3</code></pre>
<p>${esc("上と同じ間隔・同じ枠になること。")}</p>`,
  },
  {
    title: "四 ページをまたぐ長いコード",
    body: `<p>${esc("ページ送りをしても枠の左右が途切れないこと。")}</p>
${hl(Array.from({ length: 40 }, (_, i) => `line ${String(i + 1).padStart(2, "0")}: ${"x".repeat(20)}`))}
<p>${esc("おわり。")}</p>`,
  },
];

// --- ja_aozora_ruby.epub ----------------------------------------------------
// 青空文庫の新字旧仮名作品（夏目漱石『三四郎』作品ID 58842 など）では、底本の編集部が
// 補ったルビが「〔　〕」で括られて <rt> に入っている（issue #154）。
// 本文は同作品の XHTML から抜粋し、マークアップも青空文庫の形式（rb / rp / rt）に揃える。

// 青空文庫 XHTML の形式
function aoRuby(base: string, ruby: string): string {
  return `<ruby><rb>${esc(base)}</rb><rp>（</rp><rt>${ruby}</rt><rp>）</rp></ruby>`;
}
// AozoraEpub3 などの変換ツールが出す rb / rp の無い形式
function plainRuby(base: string, ruby: string): string {
  return `<ruby>${esc(base)}<rt>${ruby}</rt></ruby>`;
}

const aozoraRubyChapters: { title: string; body: string }[] = [
  {
    title: "一 〔〕で括られたルビ",
    body: [
      `<p>此爺さんは${aoRuby("慥", "〔たし〕")}かに前の前の駅から乗つた${
        aoRuby("田舎者", "いなかもの")
      }である。</p>`,
      `<p>${aoRuby("傍", "そば")}を離れるのが大いに${aoRuby("難有", "〔ありがた〕")}かつた。</p>`,
      `<p>顔は${aoRuby("生憎", "〔あいにく〕")}列車の${aoRuby("外", "そと")}に出てゐた。</p>`,
      `<p>通り掛りの${aoRuby("群集", "〔くん〕じゆ")}を眺めた。（ルビの途中に〔〕）</p>`,
      `<p>${plainRuby("屹度", "〔きっと〕")}帰つて来る。（rb / rp 無しの形式）</p>`,
    ].join("\n"),
  },
  {
    title: "二 ルビ以外の〔〕・注記入りルビ",
    body: [
      `<p>本文中の〔亀甲括弧〕と［角括弧］と[半角]はそのまま表示されること。</p>`,
      `<p>${
        aoRuby("vérité vraie", '※<span class="notes">［＃濁点付き片仮名エ、514-9］</span>リテ、ヴレイ')
      }と云ふ。</p>`,
      `<p>${aoRuby("記号", "［きごう］")}と${aoRuby("括弧", "[かっこ]")}のルビ。</p>`,
    ].join("\n"),
  },
];

// --- EPUB 組み立て ----------------------------------------------------------

type Chapter = { title: string; body: string };

function buildEpub(bookTitle: string, uuid: string, chapters: Chapter[], css?: string): Entry[] {
  const files = chapters.map((c, i) => ({
    id: `chapter${i + 1}`,
    href: `chapter${i + 1}.xhtml`,
    title: c.title,
    xhtml: `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head><meta charset="utf-8"/><title>${esc(c.title)}</title>${
      css ? '<link rel="stylesheet" type="text/css" href="stylesheet.css"/>' : ""
    }</head>
<body>
<h2>${esc(c.title)}</h2>
${c.body}
</body>
</html>
`,
  }));

  const contentOpf = `<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:${uuid}</dc:identifier>
    <dc:title>${esc(bookTitle)}</dc:title>
    <dc:language>ja</dc:language>
    <dc:creator>CrossPoint Reader</dc:creator>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
${css ? '    <item id="css" href="stylesheet.css" media-type="text/css"/>\n' : ""}${
    files.map((c) => `    <item id="${c.id}" href="${c.href}" media-type="application/xhtml+xml"/>`).join("\n")
  }
  </manifest>
  <spine>
${files.map((c) => `    <itemref idref="${c.id}"/>`).join("\n")}
  </spine>
</package>
`;

  const navXhtml = `<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja" lang="ja">
<head><meta charset="utf-8"/><title>目次</title></head>
<body><nav epub:type="toc"><ol>
${files.map((c) => `<li><a href="${c.href}">${esc(c.title)}</a></li>`).join("\n")}
</ol></nav></body>
</html>
`;

  const containerXml = `<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
`;

  const enc = new TextEncoder();
  return [
    { name: "mimetype", data: enc.encode("application/epub+zip"), store: true },
    { name: "META-INF/container.xml", data: enc.encode(containerXml), store: false },
    { name: "OEBPS/content.opf", data: enc.encode(contentOpf), store: false },
    { name: "OEBPS/nav.xhtml", data: enc.encode(navXhtml), store: false },
    ...files.map((c) => ({ name: `OEBPS/${c.href}`, data: enc.encode(c.xhtml), store: false })),
    ...(css ? [{ name: "OEBPS/stylesheet.css", data: enc.encode(css), store: false }] : []),
  ];
}

// --- ZIP 書き出し（mimetype は無圧縮で先頭に置く） -------------------------

type Entry = { name: string; data: Uint8Array; store: boolean };

function crc32(buf: Uint8Array): number {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) {
    c ^= buf[i];
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return ~c >>> 0;
}

function u16(v: number) {
  return new Uint8Array([v & 0xff, (v >> 8) & 0xff]);
}
function u32(v: number) {
  return new Uint8Array([v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]);
}
function cat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

async function writeZip(path: string, entries: Entry[]) {
  const enc = new TextEncoder();
  const chunks: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;

  for (const e of entries) {
    const nameBytes = enc.encode(e.name);
    const raw = e.data;
    const method = e.store ? 0 : 8;
    const body = e.store ? raw : new Uint8Array(
      await new Response(
        new Blob([raw as BlobPart]).stream().pipeThrough(new CompressionStream("deflate-raw")),
      ).arrayBuffer(),
    );
    const crc = crc32(raw);
    const local = cat([
      u32(0x04034b50), u16(20), u16(0), u16(method), u16(0), u16(0),
      u32(crc), u32(body.length), u32(raw.length), u16(nameBytes.length), u16(0), nameBytes,
    ]);
    chunks.push(local, body);
    central.push(cat([
      u32(0x02014b50), u16(20), u16(20), u16(0), u16(method), u16(0), u16(0),
      u32(crc), u32(body.length), u32(raw.length), u16(nameBytes.length), u16(0), u16(0), u16(0), u16(0),
      u32(0), u32(offset), nameBytes,
    ]));
    offset += local.length + body.length;
  }

  const centralBlob = cat(central);
  chunks.push(centralBlob);
  chunks.push(cat([
    u32(0x06054b50), u16(0), u16(0), u16(entries.length), u16(entries.length),
    u32(centralBlob.length), u32(offset), u16(0),
  ]));

  await Deno.writeFile(path, cat(chunks));
  console.log(`書き出し: ${path} (${entries.length} entries)`);
}

await writeZip(
  `${repoRoot}/test/epubs/ja_kinsoku.epub`,
  buildEpub("禁則処理テスト", "kinsoku-test-0001", kinsokuChapters),
);
await writeZip(
  `${repoRoot}/test/epubs/ja_lists.epub`,
  buildEpub("リスト表示テスト", "lists-test-0001", listChapters),
);
await writeZip(
  `${repoRoot}/test/epubs/ja_inline.epub`,
  buildEpub("インライン書式テスト", "inline-test-0001", inlineChapters),
);
await writeZip(
  `${repoRoot}/test/epubs/ja_headings.epub`,
  buildEpub("見出しのアキ テスト", "headings-test-0001", headingChapters),
);
await writeZip(
  `${repoRoot}/test/epubs/ja_pre.epub`,
  buildEpub("コードブロック テスト", "pre-test-0001", preChapters, preCss),
);
await writeZip(
  `${repoRoot}/test/epubs/ja_aozora_ruby.epub`,
  buildEpub("青空文庫ルビ テスト", "aozora-ruby-test-0001", aozoraRubyChapters),
);
// 縦書きはリーダーが spine の page-progression-direction="rtl" で判定するので、それも付ける
await writeZip(
  `${repoRoot}/test/epubs/ja_aozora_ruby_vertical.epub`,
  buildEpub(
    "青空文庫ルビ テスト（縦書き）",
    "aozora-ruby-test-0002",
    aozoraRubyChapters,
    "body { writing-mode: vertical-rl; }\n",
  ).map((e) =>
    e.name === "OEBPS/content.opf"
      ? {
        ...e,
        data: new TextEncoder().encode(
          new TextDecoder().decode(e.data).replace("<spine>", '<spine page-progression-direction="rtl">'),
        ),
      }
      : e
  ),
);
