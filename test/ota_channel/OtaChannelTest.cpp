// OTA アップデートチャネルのホストテスト。
//
// 検証対象は2つ。
//   1. ReleaseJsonParser — GitHub /releases API の2つのレスポンス形状
//      (単一オブジェクト / 配列) を同じパーサで扱えること
//   2. src/network/ReleaseVersion.h の選択ポリシー — チャネル × 端末状態 ×
//      候補集合の遷移マトリクス
//
// OtaUpdater 本体は HttpDownloader (Arduino / mbedTLS) 依存でホストではリンク
// できないため、そこから切り出した純粋関数を対象にする。実機と同じ
// shouldReplaceWinner() を呼ぶので、判定ロジックの二重実装にはならない。

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../lib/JsonParser/ReleaseJsonParser.h"
#include "../../src/network/ReleaseVersion.h"

namespace {

int failures = 0;

void check(const bool ok, const std::string& what) {
  if (!ok) {
    printf("  FAIL: %s\n", what.c_str());
    failures++;
  }
}

void checkEq(const std::string& actual, const std::string& expected, const std::string& what) {
  if (actual != expected) {
    printf("  FAIL: %s\n        expected: %s\n        actual:   %s\n", what.c_str(), expected.c_str(), actual.c_str());
    failures++;
  }
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    printf("  FAIL: cannot open fixture %s\n", path.c_str());
    failures++;
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Emitted {
  std::string tag;
  std::string url;
  size_t size;
};

std::vector<Emitted> emitted;

void onRelease(void*, const char* tag, const char* fwUrl, size_t fwSize) { emitted.push_back({tag, fwUrl, fwSize}); }

// Feeds the body in fixed-size chunks so token and structural boundaries land
// mid-buffer, which is what the device sees coming off TLS.
std::vector<Emitted> parseAll(const std::string& body, const size_t chunk) {
  emitted.clear();
  ReleaseJsonParser parser;
  parser.setHandler(&onRelease, nullptr);
  parser.resetStream();
  for (size_t i = 0; i < body.size(); i += chunk) {
    parser.feed(body.data() + i, std::min(chunk, body.size() - i));
  }
  return emitted;
}

std::string joinTags(const std::vector<Emitted>& list) {
  std::string s;
  for (const auto& e : list) {
    if (!s.empty()) s += ",";
    s += e.tag;
  }
  return s;
}

// -- 1. パーサ --------------------------------------------------------------

void testParserShapes(const std::string& dir) {
  printf("parser: response shapes\n");

  const std::string single = readFile(dir + "/releases_latest.json");
  const std::string list = readFile(dir + "/releases_list.json");
  if (single.empty() || list.empty()) return;

  // 1バイト刻みまで含めた複数のチャンクサイズで結果が変わらないこと。
  for (const size_t chunk : {1u, 7u, 64u, 1024u, 65536u}) {
    const auto one = parseAll(single, chunk);
    check(one.size() == 1, "single object emits exactly one release (chunk=" + std::to_string(chunk) + ")");
    if (one.size() == 1) {
      checkEq(one[0].tag, "v0.1.17", "single object tag (chunk=" + std::to_string(chunk) + ")");
      check(one[0].url.find("/releases/download/v0.1.17/firmware.bin") != std::string::npos,
            "single object firmware url (chunk=" + std::to_string(chunk) + ")");
      check(one[0].size > 0, "single object firmware size (chunk=" + std::to_string(chunk) + ")");
    }

    const auto many = parseAll(list, chunk);
    checkEq(joinTags(many),
            "dev-20260821-122514,dev-20260820-131454,dev-20260819-123940,v0.1.17,dev-20260818-124704,v0.1.16,"
            "dev-20260803-235626,dev-20260803-132059,dev-20260802-131118,dev-20260802-080012",
            "array emits every release in order (chunk=" + std::to_string(chunk) + ")");
  }
}

void testParserSkipsUninstallable(const std::string& dir) {
  printf("parser: uninstallable releases\n");

  const std::string body = readFile(dir + "/releases_synthetic.json");
  if (body.empty()) return;

  const auto list = parseAll(body, 512);
  // sd-fonts (firmware.bin なし) と notes-only (アセットなし) は届かない。
  // タグの形が未知の v0.2.0-beta1 はパーサでは弾かず、上位で落とす。
  checkEq(joinTags(list), "dev-20260822-090000,v0.1.18-rc2,v0.1.18-rc1,v0.2.0-beta1,v0.1.17,dev-20260817-100000",
          "releases without firmware.bin are not emitted");
}

void testParserCountsAcrossResponses(const std::string& dir) {
  printf("parser: release count across responses\n");

  const std::string single = readFile(dir + "/releases_latest.json");
  const std::string list = readFile(dir + "/releases_list.json");
  if (single.empty() || list.empty()) return;

  emitted.clear();
  ReleaseJsonParser parser;
  parser.setHandler(&onRelease, nullptr);

  parser.resetStream();
  parser.feed(single.data(), single.size());
  check(parser.releaseCount() == 1, "count after the first response");

  // 2フェッチ構成の要: ストリーム状態だけリセットし、件数は積み上がる。
  parser.resetStream();
  parser.feed(list.data(), list.size());
  check(parser.releaseCount() == 11, "count accumulates across responses");
  check(emitted.size() == 11, "handler fires for every release of both responses");

  parser.reset();
  check(parser.releaseCount() == 0, "reset() clears the count");
}

void testParserRejectsGarbage() {
  printf("parser: non-release payloads\n");

  // GitHub のエラーレスポンス。release オブジェクトではないので件数は 0 のまま。
  const std::string err = R"({"message":"Not Found","documentation_url":"https://docs.github.com/rest"})";
  emitted.clear();
  ReleaseJsonParser parser;
  parser.setHandler(&onRelease, nullptr);
  parser.resetStream();
  parser.feed(err.data(), err.size());
  // オブジェクト1個ぶんは数えるが、tag も firmware も無いので何も emit しない。
  check(emitted.empty(), "error payload emits nothing");

  const std::string empty = "[]";
  ReleaseJsonParser p2;
  p2.setHandler(&onRelease, nullptr);
  emitted.clear();
  p2.resetStream();
  p2.feed(empty.data(), empty.size());
  check(p2.releaseCount() == 0, "empty array counts zero releases");
  check(emitted.empty(), "empty array emits nothing");
}

// -- 2. タグ / バージョンのパース ------------------------------------------

void testTagParsing() {
  printf("version: candidate tags\n");
  ReleaseVersionKey k;

  check(releaseVersion::parseCandidateTag("v0.1.17", &k) && !k.isDev && k.major == 0 && k.minor == 1 && k.patch == 17 &&
            k.rc == 0,
        "v0.1.17");
  check(releaseVersion::parseCandidateTag("v1.20.3", &k) && k.major == 1 && k.minor == 20 && k.patch == 3, "v1.20.3");
  check(releaseVersion::parseCandidateTag("v0.1.18-rc2", &k) && k.rc == 2 && k.patch == 18, "v0.1.18-rc2");
  check(releaseVersion::parseCandidateTag("dev-20260821-122514", &k) && k.isDev &&
            strcmp(k.devStamp, "20260821-122514") == 0,
        "dev-20260821-122514");

  // 末尾までアンカーされているので、未知の名前空間は候補にならない。
  check(!releaseVersion::parseCandidateTag("sd-fonts", &k), "sd-fonts rejected");
  check(!releaseVersion::parseCandidateTag("v0.2.0-beta1", &k), "v0.2.0-beta1 rejected");
  check(!releaseVersion::parseCandidateTag("v0.1.17-3-gabc1234", &k), "off-tag describe form rejected as a candidate");
  check(!releaseVersion::parseCandidateTag("v0.1", &k), "two-segment tag rejected");
  check(!releaseVersion::parseCandidateTag("dev-2026821-122514", &k), "short dev stamp rejected");
  check(!releaseVersion::parseCandidateTag("dev-20260821_122514", &k), "dev stamp separator enforced");
  check(!releaseVersion::parseCandidateTag("dev-2026082a-122514", &k), "non-digit dev stamp rejected");
  check(!releaseVersion::parseCandidateTag("v0.1.18-rc0", &k), "rc0 rejected");
  check(!releaseVersion::parseCandidateTag("v0.1.18-rc1x", &k), "trailing junk after rc rejected");
  // 桁数を縛っていないと、device (RISC-V ILP32, LONG_MAX == INT_MAX) では
  // strtol が飽和して 2147483647 として通ってしまい、ホストと挙動が食い違う。
  check(!releaseVersion::parseCandidateTag("v99999999999.0.0", &k), "over-long version segment rejected");
  check(!releaseVersion::parseCandidateTag("v0.1.18-rc99999999999", &k), "over-long rc ordinal rejected");
  check(releaseVersion::parseCandidateTag("v999999999.0.0", &k) && k.major == 999999999,
        "nine-digit segment still accepted");
  check(!releaseVersion::parseCandidateTag("", &k), "empty tag rejected");
  check(!releaseVersion::parseCandidateTag(nullptr, &k), "null tag rejected");
}

void testDeviceVersionParsing() {
  printf("version: device versions\n");
  ReleaseVersionKey k;

  check(releaseVersion::parseDeviceVersion("0.1.17", &k) && !k.offTag && k.rc == 0, "0.1.17 is on-tag");
  check(releaseVersion::parseDeviceVersion("0.1.18-rc1", &k) && !k.offTag && k.rc == 1, "0.1.18-rc1 is on-tag");
  check(releaseVersion::parseDeviceVersion("0.1.17-3-gabc1234", &k) && k.offTag && k.rc == 0, "dev build is off-tag");
  check(releaseVersion::parseDeviceVersion("0.1.18-rc1-3-gabc1234", &k) && k.offTag && k.rc == 1,
        "rc and off-tag coexist");
  check(releaseVersion::parseDeviceVersion("0.1.17-dev-feature/91-ota-abc1234", &k) && k.offTag,
        "local build with a slash in the branch is off-tag");
  check(!releaseVersion::parseDeviceVersion("unknown", &k), "unparseable version rejected");
  check(releaseVersion::parseDeviceVersion("0.1.18-rc99999999999-3-gabc", &k) && k.rc == 0 && k.offTag,
        "over-long rc ordinal on the device falls back to offTag");
  check(!releaseVersion::parseDeviceVersion("", &k), "empty version rejected");
}

// -- 3. 遷移マトリクス ------------------------------------------------------

// 実機の checkForUpdate() と同じ手順: 候補を順に流し、勝者を1つだけ保持する。
std::string selectWinner(const std::vector<std::string>& tags, const char* deviceVersion, const char* deviceBuildTime,
                         const releaseVersion::Channel channel) {
  ReleaseVersionKey device;
  releaseVersion::parseDeviceVersion(deviceVersion, &device);

  ReleaseVersionKey winner;
  std::string winnerTag;
  for (const auto& tag : tags) {
    ReleaseVersionKey key;
    if (!releaseVersion::parseCandidateTag(tag.c_str(), &key)) continue;
    if (!releaseVersion::shouldReplaceWinner(key, winner, device, deviceBuildTime, channel)) continue;
    winner = key;
    winnerTag = tag;
  }
  return winnerTag.empty() ? "(none)" : winnerTag;
}

void testSelectionMatrix() {
  printf("selection: channel x device state\n");

  // /releases/latest + /releases?per_page=10 を連結した候補集合。順序は
  // GitHub が返す順（publish 時系列とは一致しない）をそのまま模す。
  const std::vector<std::string> tags = {
      "v0.1.17",              // /releases/latest
      "dev-20260822-090000",  // ここから /releases
      "v0.1.18-rc2",
      "v0.1.18-rc1",
      "sd-fonts",
      "v0.2.0-beta1",
      "v0.1.17",
      "dev-20260817-100000",
  };
  constexpr auto STABLE = releaseVersion::CHANNEL_STABLE;
  constexpr auto RC = releaseVersion::CHANNEL_RC;
  constexpr auto DEV = releaseVersion::CHANNEL_DEV;

  // 正式リリースちょうどの端末。
  checkEq(selectWinner(tags, "0.1.17", "20260818-124126", STABLE), "(none)", "release device, STABLE: nothing newer");
  checkEq(selectWinner(tags, "0.1.17", "20260818-124126", RC), "v0.1.18-rc2", "release device, RC: highest rc");
  checkEq(selectWinner(tags, "0.1.17", "20260818-124126", DEV), "dev-20260822-090000",
          "release device, DEV: newest dev build");

  // RC 上の端末。
  checkEq(selectWinner(tags, "0.1.18-rc1", "20260820-100000", STABLE), "(none)",
          "rc device, STABLE: waits for v0.1.18");
  checkEq(selectWinner(tags, "0.1.18-rc1", "20260820-100000", RC), "v0.1.18-rc2", "rc device, RC: next rc");
  checkEq(selectWinner(tags, "0.1.18-rc1", "20260820-100000", DEV), "dev-20260822-090000",
          "rc device, DEV: newest dev build");

  // Dev Build 上の端末（最新ではない）。
  checkEq(selectWinner(tags, "0.1.17-3-gabc1234", "20260817-100000", STABLE), "v0.1.17",
          "stale dev device, STABLE: escape hatch back to the release");
  checkEq(selectWinner(tags, "0.1.17-3-gabc1234", "20260817-100000", RC), "v0.1.18-rc2",
          "stale dev device, RC: highest rc");
  checkEq(selectWinner(tags, "0.1.17-3-gabc1234", "20260817-100000", DEV), "dev-20260822-090000",
          "stale dev device, DEV: newest dev build");

  // 最新 Dev Build 上の端末。RC タグが存在する状態で master から作られた
  // Dev Build は git describe が RC を拾うため "0.1.18-rc2-2-g…" になる
  // (scripts/git_branch.py)。この端末はどのチャネルでも更新なしに落ち着く。
  checkEq(selectWinner(tags, "0.1.18-rc2-2-gdef5678", "20260822-090000", DEV), "(none)",
          "newest dev device, DEV: self-excluded, no ping-pong");
  // RC チャネルでは脱出口が働き、いま乗っている Dev Build のベースタグ
  // (v0.1.18-rc2) に着地する。install 後は on-tag になるので1回で収束する。
  checkEq(selectWinner(tags, "0.1.18-rc2-2-gdef5678", "20260822-090000", RC), "v0.1.18-rc2",
          "newest dev device, RC: escape hatch onto its base rc tag");
  // STABLE では v0.1.17 が semver 上ほんとうに古いので降格させない。
  // v0.1.18 が出るまで更新なし。
  checkEq(selectWinner(tags, "0.1.18-rc2-2-gdef5678", "20260822-090000", STABLE), "(none)",
          "newest dev device, STABLE: no downgrade below its base tag");

  // RC がまだ無い時期（正式リリースのみが存在）の最新 Dev Build。
  const std::vector<std::string> releasesOnly = {"v0.1.17", "dev-20260822-090000", "dev-20260817-100000", "v0.1.16"};
  checkEq(selectWinner(releasesOnly, "0.1.17-9-gdef5678", "20260822-090000", DEV), "(none)",
          "newest dev device, DEV: self-excluded");
  checkEq(selectWinner(releasesOnly, "0.1.17-9-gdef5678", "20260822-090000", STABLE), "v0.1.17",
          "newest dev device, STABLE: escape hatch onto the release line");

  // 正式版が dev より新しい場合、DEV でも dev が勝つ（dev は master 由来で
  // リリース済みコミットを必ず含むため）。逆に dev が全て古ければ正式が勝つ。
  const std::vector<std::string> onlyOldDev = {"v0.1.18", "dev-20260101-000000"};
  checkEq(selectWinner(onlyOldDev, "0.1.17", "20260817-100000", DEV), "v0.1.18",
          "DEV falls back to the release when every dev build is older");

  // 未知の名前空間・firmware なしタグは決して勝たない。
  const std::vector<std::string> noise = {"sd-fonts", "v0.2.0-beta1", "notes-only"};
  checkEq(selectWinner(noise, "0.1.17", "20260817-100000", DEV), "(none)", "unknown tag namespaces never win");

  // バージョン不明の端末では、リリース候補は選ばれず dev 候補だけが通る。
  checkEq(selectWinner(tags, "unknown", "20260817-100000", STABLE), "(none)",
          "unparseable device version blocks release candidates");
  checkEq(selectWinner(tags, "unknown", "20260817-100000", DEV), "dev-20260822-090000",
          "unparseable device version still allows dev builds");

  // 埋め込みスタンプが無い（古いファームや local.ini ピン）端末は、
  // BuildInfo.h のフォールバック値で全 dev build より小さくなる。
  checkEq(selectWinner(tags, "0.1.17", "00000000-000000", DEV), "dev-20260822-090000",
          "missing build stamp treats every dev build as newer");
}

// 途中で切れたレスポンスでパーサが壊れないこと。TLS ストリームは切断されうるし、
// GitHub がエラーボディを返すこともある。ASan/UBSan ビルドで走らせると
// 配列外アクセスもここで捕まる。
void testParserTruncatedAndGarbage(const std::string& dir) {
  printf("parser: truncated and garbage input\n");

  const std::string body = readFile(dir + "/releases_list.json");
  if (body.empty()) return;

  // 全ての長さで打ち切る。emit された分は必ず正常な prefix になっているはず。
  const auto full = parseAll(body, 4096);
  for (size_t len = 0; len <= body.size(); len += 97) {
    const auto partial = parseAll(body.substr(0, len), 512);
    if (partial.size() > full.size()) {
      check(false, "truncated input emitted more releases than the full body at len=" + std::to_string(len));
      break;
    }
    bool prefixOk = true;
    for (size_t i = 0; i < partial.size(); i++) {
      if (partial[i].tag != full[i].tag) prefixOk = false;
    }
    if (!prefixOk) {
      check(false, "truncated input emitted a different release sequence at len=" + std::to_string(len));
      break;
    }
  }

  // 決定的な擬似乱数バイト列。クラッシュしないことだけを見る。
  uint32_t seed = 0xC0FFEEu;
  for (int trial = 0; trial < 64; trial++) {
    std::string junk;
    junk.reserve(2048);
    for (int i = 0; i < 2048; i++) {
      seed = seed * 1664525u + 1013904223u;
      // JSON の構造文字を濃くして状態機械を深く踏ませる
      static const char alphabet[] = "{}[]\":,0123456789abcdefgtruefalsnl \\\n";
      junk.push_back(alphabet[(seed >> 16) % (sizeof(alphabet) - 1)]);
    }
    parseAll(junk, 64);
  }

  // 深いネスト。StreamingJsonParser の MAX_NESTING(32) を超える入力。
  std::string deep = "{\"tag_name\":\"v9.9.9\",\"assets\":[";
  for (int i = 0; i < 64; i++) deep += "[";
  for (int i = 0; i < 64; i++) deep += "]";
  deep += "]}";
  parseAll(deep, 16);

  check(true, "parser survived truncated, garbage and deeply nested input");
}

// -- 4. 性質テスト ----------------------------------------------------------

// 勝者を選ぶ順序が GitHub のレスポンス順に依存しないこと。
// /releases は publish 時系列で並んでおらず順序逆転が実際に起きるため、
// どの順で候補が届いても同じ勝者になることが要件になる。
void testOrderIndependence() {
  printf("property: winner is independent of arrival order\n");

  const std::vector<std::string> base = {"v0.1.17",   "dev-20260822-090000", "v0.1.18-rc2", "v0.1.18-rc1",
                                         "sd-fonts",  "v0.2.0-beta1",        "v0.1.16",     "dev-20260817-100000",
                                         "notes-only"};
  const struct {
    const char* version;
    const char* stamp;
  } devices[] = {
      {"0.1.17", "20260818-124126"},
      {"0.1.18-rc1", "20260820-100000"},
      {"0.1.17-3-gabc1234", "20260817-100000"},
      {"0.1.18-rc2-2-gdef5678", "20260822-090000"},
      {"unknown", "00000000-000000"},
  };

  // 決定的な擬似乱数で並べ替える。テストの再現性を壊さないため rand() は使わない。
  uint32_t seed = 0x12345678u;
  auto nextRand = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 16;
  };

  for (const auto& dev : devices) {
    for (const auto channel :
         {releaseVersion::CHANNEL_STABLE, releaseVersion::CHANNEL_RC, releaseVersion::CHANNEL_DEV}) {
      const std::string expected = selectWinner(base, dev.version, dev.stamp, channel);
      for (int trial = 0; trial < 200; trial++) {
        std::vector<std::string> shuffled = base;
        for (size_t i = shuffled.size(); i > 1; i--) {
          std::swap(shuffled[i - 1], shuffled[nextRand() % i]);
        }
        const std::string got = selectWinner(shuffled, dev.version, dev.stamp, channel);
        if (got != expected) {
          checkEq(got, expected,
                  std::string("order independence: device=") + dev.version + " channel=" + std::to_string(channel));
          break;  // 1件報告すれば十分
        }
      }
    }
  }
}

// 端末が2つのビルドを往復し続けない（ping-pong しない）こと。
// 案1・案2がここで破綻した。勝者をインストールした後の端末状態を作り、
// 「更新なし」に到達するか、同じ状態に戻ってこないかを確認する。
void testConvergence() {
  printf("property: repeated installs converge (no ping-pong)\n");

  const std::vector<std::string> candidates = {"v0.1.17", "v0.1.18-rc1", "v0.1.18-rc2", "dev-20260822-090000",
                                               "dev-20260817-100000"};

  // インストール後の端末状態をモデル化する。
  //  - dev タグ: 版はその時点で master にある最上位の v タグ + distance
  //    （git describe の挙動。RCタグが打たれた後の dev build は
  //     "0.1.18-rc2-2-g…" になる）。埋め込み時刻はタグのスタンプ
  //  - v タグ: 版はそのタグちょうど（off-tag ではない）。埋め込み時刻は
  //    リリースビルドの壁時計で、こちらは候補集合から決まらないので
  //    「全 dev より古い」「全 dev より新しい」の両極端で試す
  const char* highestReleaseTag = "v0.1.18-rc2";

  for (const char* releaseStamp : {"20260101-000000", "20260901-000000"}) {
    for (const auto channel :
         {releaseVersion::CHANNEL_STABLE, releaseVersion::CHANNEL_RC, releaseVersion::CHANNEL_DEV}) {
      for (const char* startVersion : {"0.1.16", "0.1.17", "0.1.18-rc1", "0.1.17-3-gabc", "0.1.18-rc2-2-gdef"}) {
        std::string version = startVersion;
        std::string stamp = "20260817-100000";
        std::vector<std::string> seen;

        for (int step = 0; step < 12; step++) {
          const std::string key = version + "@" + stamp;
          bool cycled = false;
          for (const auto& s : seen) {
            if (s == key) cycled = true;
          }
          if (cycled) {
            check(false, std::string("ping-pong: start=") + startVersion + " channel=" + std::to_string(channel) +
                             " releaseStamp=" + releaseStamp + " revisited " + key);
            break;
          }
          seen.push_back(key);

          const std::string winner = selectWinner(candidates, version.c_str(), stamp.c_str(), channel);
          if (winner == "(none)") break;  // 収束

          if (winner.rfind("dev-", 0) == 0) {
            version = std::string(highestReleaseTag).substr(1) + "-2-gsim";
            stamp = winner.substr(4);
          } else {
            version = winner.substr(1);
            stamp = releaseStamp;
          }
        }
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "test/ota_channel";

  testParserShapes(dir);
  testParserSkipsUninstallable(dir);
  testParserCountsAcrossResponses(dir);
  testParserRejectsGarbage();
  testParserTruncatedAndGarbage(dir);
  testTagParsing();
  testDeviceVersionParsing();
  testSelectionMatrix();
  testOrderIndependence();
  testConvergence();

  if (failures == 0) {
    printf("\nAll OTA channel tests passed.\n");
    return 0;
  }
  printf("\n%d assertion(s) failed.\n", failures);
  return 1;
}
