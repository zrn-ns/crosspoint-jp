#!/usr/bin/env -S deno run --allow-read --allow-write --allow-run --allow-env
// ホストシミュレータ（crosspoint-simulator フォーク）を、決まった手順で動かして
// スクリーンショットとログ要約を残すためのランナー。
//
//   deno run -A scripts/sim_run.ts --build --sd-font BIZUDGothic \
//     --open /Books/ja_vertical.epub --shot 6000:page
//
// やること:
//   1. SD ディレクトリを用意する（既定: .pio/sim-sd/ に test/epubs と .cpfont を集める）
//   2. 必要なら pio run -e <env> でビルドする
//   3. CROSSPOINT_SIM_* を組んで .pio/build/<env>/program を起動する（タイムアウト付き）
//   4. BMP を PNG に変換し（macOS の sips）、ログから ERR / Outside range / Activity 遷移を要約する
//
// 終了コード: シミュレータが異常終了したら非 0。

import { parseArgs } from "jsr:@std/cli@1/parse-args";
import { copy, ensureDir, exists } from "jsr:@std/fs@1";
import { join, resolve } from "jsr:@std/path@1";

const HELP = `sim_run.ts — CrossPoint ホストシミュレータのランナー

使い方:
  deno run -A scripts/sim_run.ts [options]

環境:
  --env <name>          simulator | simulator_x3（既定: simulator）
  --build               先に pio run -e <env> を実行する
  --timeout <sec>       シミュレータの最大実行時間（既定: 60）

SD カード（既定は .pio/sim-sd/ を組み立てる）:
  --sd <dir>            既存のディレクトリを SD として使う（実機 SD の /Volumes/xxx も可。書き込みが起きるので注意）
  --sd-font <Family>    lib/EpdFont/scripts/output/<Family> の .cpfont を SD に入れ、
                        settings.json の horizontal/vertical.sdFontFamilyName に設定する
  --setting k=v         settings.json のトップレベルキーを上書き（複数可。数値/true/false は型変換）
  --open </path.epub>   起動直後にその本を開く（state.json の openEpubPath + lastSleepFromReader）
  --reset-cache         SD の .crosspoint/epub_* / xtc_* を消してから起動する
  --reset-settings      SD の .crosspoint/settings.json と state.json を消してから起動する

操作とスクリーンショット:
  --script '<ms>:<KEY>[:<hold>];...'   CROSSPOINT_SIM_INPUT_SCRIPT。KEY = BACK ENTER LEFT RIGHT UP DOWN POWER SLEEP HOME QUIT
                                       QUIT が無ければ --quit-at で自動追加する
  --quit-at <ms>        QUIT を打つ時刻（既定: 最後のイベント + 3000）
  --shot <ms>:<name>    その時刻にスクリーンショット（複数可）。<out>/<name>.png に保存
  --out <dir>           出力先（既定: .pio/sim-out/<timestamp>/）
  --heap <bytes>        CROSSPOINT_SIM_FREE_HEAP（低ヒープ分岐の再現）

例:
  # Home のスクリーンショットだけ
  deno run -A scripts/sim_run.ts --shot 1500:home
  # 縦書き EPUB を SD フォントで開いて本文まで
  deno run -A scripts/sim_run.ts --sd-font BIZUDGothic --open /Books/ja_vertical.epub --shot 6000:page
`;

const args = parseArgs(Deno.args, {
  boolean: ["build", "help", "reset-cache", "reset-settings"],
  string: ["env", "timeout", "sd", "sd-font", "open", "script", "quit-at", "out", "heap"],
  collect: ["shot", "setting"],
  alias: { h: "help" },
  default: { env: "simulator", timeout: "60" },
});
if (args.help) {
  console.log(HELP);
  Deno.exit(0);
}

const repoRoot = resolve(new URL("..", import.meta.url).pathname);
const env = String(args.env);
const program = join(repoRoot, ".pio", "build", env, "program");

// ---------------------------------------------------------------------------
// SD ディレクトリ
// ---------------------------------------------------------------------------
async function assembleDefaultSd(): Promise<string> {
  const sd = join(repoRoot, ".pio", "sim-sd");
  await ensureDir(join(sd, "Books"));
  await ensureDir(join(sd, ".crosspoint"));
  // テスト EPUB をコピー（既にあればスキップ）
  for await (const e of Deno.readDir(join(repoRoot, "test", "epubs"))) {
    if (!e.isFile || !e.name.endsWith(".epub")) continue;
    const dst = join(sd, "Books", e.name);
    if (!(await exists(dst))) await Deno.copyFile(join(repoRoot, "test", "epubs", e.name), dst);
  }
  return sd;
}

const sd = args.sd ? resolve(String(args.sd)) : await assembleDefaultSd();
if (!(await exists(sd))) {
  console.error(`error: SD ディレクトリがありません: ${sd}`);
  Deno.exit(2);
}
const crosspointDir = join(sd, ".crosspoint");
await ensureDir(crosspointDir);

if (args["reset-cache"]) {
  for await (const e of Deno.readDir(crosspointDir)) {
    if (e.isDirectory && (e.name.startsWith("epub_") || e.name.startsWith("xtc_"))) {
      await Deno.remove(join(crosspointDir, e.name), { recursive: true });
    }
  }
}
if (args["reset-settings"]) {
  for (const f of ["settings.json", "state.json"]) {
    try {
      await Deno.remove(join(crosspointDir, f));
    } catch { /* 無ければよい */ }
  }
}

// SD フォント
if (args["sd-font"]) {
  const family = String(args["sd-font"]);
  const src = join(repoRoot, "lib", "EpdFont", "scripts", "output", family);
  const dst = join(crosspointDir, "fonts", family);
  if (!(await exists(dst))) {
    if (!(await exists(src))) {
      console.error(`error: .cpfont が見つかりません: ${src}（lib/EpdFont/scripts/build-sd-fonts.py で生成する）`);
      Deno.exit(2);
    }
    await ensureDir(join(crosspointDir, "fonts"));
    await copy(src, dst);
  }
}

// settings.json / state.json
type Json = Record<string, unknown>;
async function readJson(path: string): Promise<Json> {
  try {
    return JSON.parse(await Deno.readTextFile(path)) as Json;
  } catch {
    return {};
  }
}
function coerce(v: string): unknown {
  if (v === "true") return true;
  if (v === "false") return false;
  if (/^-?\d+(\.\d+)?$/.test(v)) return Number(v);
  return v;
}
{
  const settingsPath = join(crosspointDir, "settings.json");
  const settings = await readJson(settingsPath);
  let dirty = false;
  if (args["sd-font"]) {
    for (const dir of ["horizontal", "vertical"]) {
      const obj = (settings[dir] as Json | undefined) ?? {};
      obj.sdFontFamilyName = String(args["sd-font"]);
      settings[dir] = obj;
    }
    dirty = true;
  }
  for (const kv of (args.setting as string[]) ?? []) {
    const i = kv.indexOf("=");
    if (i < 0) {
      console.error(`error: --setting は key=value の形で: ${kv}`);
      Deno.exit(2);
    }
    settings[kv.slice(0, i)] = coerce(kv.slice(i + 1));
    dirty = true;
  }
  if (dirty) await Deno.writeTextFile(settingsPath, JSON.stringify(settings));

  if (args.open) {
    const statePath = join(crosspointDir, "state.json");
    const state = await readJson(statePath);
    // main.cpp は openEpubPath && lastSleepFromReader のとき Home を飛ばして直接リーダーを開く
    state.openEpubPath = String(args.open);
    state.lastSleepFromReader = true;
    state.readerActivityLoadCount = 0; // 前回クラッシュ扱いで Home に落とされないように
    await Deno.writeTextFile(statePath, JSON.stringify(state));
  }
}

// ---------------------------------------------------------------------------
// ビルド
// ---------------------------------------------------------------------------
if (args.build || !(await exists(program))) {
  console.log(`[sim_run] pio run -e ${env}`);
  const p = new Deno.Command("pio", { args: ["run", "-e", env], cwd: repoRoot, stdout: "piped", stderr: "piped" });
  const out = await p.output();
  const text = new TextDecoder().decode(out.stdout) + new TextDecoder().decode(out.stderr);
  if (!out.success) {
    // リンクエラーは lib/hal の API 変更にフォーク側スタブが追従していない兆候
    console.error(text.split("\n").filter((l) => /error|Undefined|referenced from|FAILED/.test(l)).join("\n"));
    console.error(
      "\n[sim_run] ビルド失敗。未定義シンボルなら zrn-ns/crosspoint-simulator#crosspoint-jp にスタブを足す。",
    );
    Deno.exit(1);
  }
  console.log(text.split("\n").filter((l) => /SUCCESS|Took/.test(l)).join("\n"));
}

// ---------------------------------------------------------------------------
// 入力スクリプトとスクリーンショット
// ---------------------------------------------------------------------------
const outDir = args.out
  ? resolve(String(args.out))
  : join(repoRoot, ".pio", "sim-out", new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19));
await ensureDir(outDir);

const events: { ms: number; text: string }[] = [];
if (args.script) {
  for (const part of String(args.script).split(";")) {
    const m = part.trim().match(/^(\d+):(.+)$/);
    if (!m) {
      console.error(`error: --script の書式が不正: ${part}`);
      Deno.exit(2);
    }
    events.push({ ms: Number(m[1]), text: m[2] });
  }
}
const shots: { ms: number; name: string }[] = [];
for (const s of (args.shot as string[]) ?? []) {
  const m = s.match(/^(\d+):([\w.-]+)$/);
  if (!m) {
    console.error(`error: --shot は <ms>:<name> の形で: ${s}`);
    Deno.exit(2);
  }
  shots.push({ ms: Number(m[1]), name: m[2] });
}
if (!events.some((e) => e.text.startsWith("QUIT"))) {
  const last = Math.max(0, ...events.map((e) => e.ms), ...shots.map((s) => s.ms));
  const quitAt = args["quit-at"] ? Number(args["quit-at"]) : last + 3000;
  events.push({ ms: quitAt, text: "QUIT" });
}
events.sort((a, b) => a.ms - b.ms);

const runEnv: Record<string, string> = {
  CROSSPOINT_SIM_SD: sd,
  CROSSPOINT_SIM_INPUT_SCRIPT: events.map((e) => `${e.ms}:${e.text}`).join(";"),
};
if (shots.length) {
  runEnv.CROSSPOINT_SIM_SCREENSHOTS = shots.map((s) => `${s.ms}:${join(outDir, s.name + ".bmp")}`).join(";");
}
if (args.heap) runEnv.CROSSPOINT_SIM_FREE_HEAP = String(args.heap);

console.log(`[sim_run] sd=${sd}`);
console.log(`[sim_run] script=${runEnv.CROSSPOINT_SIM_INPUT_SCRIPT}`);
console.log(`[sim_run] out=${outDir}`);

// ---------------------------------------------------------------------------
// 実行
// ---------------------------------------------------------------------------
const timeoutMs = Number(args.timeout) * 1000;
const child = new Deno.Command(program, { cwd: repoRoot, env: runEnv, stdout: "piped", stderr: "piped" }).spawn();
const killer = setTimeout(() => {
  console.error(`[sim_run] timeout ${args.timeout}s — kill`);
  try {
    child.kill("SIGKILL");
  } catch { /* already gone */ }
}, timeoutMs);
const result = await child.output();
clearTimeout(killer);
const log = new TextDecoder().decode(result.stdout) + new TextDecoder().decode(result.stderr);
const logPath = join(outDir, "sim.log");
await Deno.writeTextFile(logPath, log);

// ---------------------------------------------------------------------------
// 要約
// ---------------------------------------------------------------------------
const lines = log.split("\n");
const activities = lines.filter((l) => /\[ACT\] (Entering|Exiting) activity/.test(l)).map((l) =>
  l.replace(/^\[\d+\] \[DBG\] \[ACT\] /, "")
);
const errs = lines.filter((l) => /\[ERR\]/.test(l) && !/outside range/i.test(l));
const outside = lines.filter((l) => /Outside range/.test(l)).length;
const sanitizer = lines.filter((l) => /runtime error|AddressSanitizer|SUMMARY:/.test(l));

console.log("\n=== Activity ===");
for (const a of activities) console.log("  " + a);
console.log(`\n=== ERR (${errs.length}) ===`);
for (const e of errs.slice(0, 20)) console.log("  " + e);
if (outside) console.log(`\n!! 画面外描画 (Outside range) ${outside} 行 — レイアウトのはみ出し。ログ: ${logPath}`);
if (sanitizer.length) {
  console.log("\n!! sanitizer:");
  for (const s of sanitizer.slice(0, 10)) console.log("  " + s);
}

// BMP → PNG（macOS の sips。無ければ BMP のまま）
console.log("\n=== Screenshots ===");
for (const s of shots) {
  const bmp = join(outDir, s.name + ".bmp");
  if (!(await exists(bmp))) {
    console.log(`  ${s.name}: 取得できず（${s.ms}ms までにプロセスが終了した可能性）`);
    continue;
  }
  const png = join(outDir, s.name + ".png");
  try {
    const conv = await new Deno.Command("sips", {
      args: ["-s", "format", "png", bmp, "--out", png],
      stdout: "null",
      stderr: "null",
    }).output();
    if (conv.success) {
      await Deno.remove(bmp);
      console.log(`  ${png}`);
      continue;
    }
  } catch { /* sips 無し */ }
  console.log(`  ${bmp}`);
}

const crashed = !result.success && result.signal !== null && result.signal !== "SIGKILL" && result.signal !== "SIGTERM";
console.log(`\n[sim_run] exit code=${result.code} signal=${result.signal ?? "-"} ${crashed ? "!! CRASH" : ""}`);
Deno.exit(result.success || result.signal === "SIGKILL" || result.signal === "SIGTERM" ? (crashed ? 1 : 0) : 1);
