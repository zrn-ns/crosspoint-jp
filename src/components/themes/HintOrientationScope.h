#pragma once

#include <GfxRenderer.h>

// ボタンヒントを描く間だけ、レンダラの向きを Portrait に固定する RAII ヘルパ。
//
// 前面ボタンは縦持ちでの下端（短辺側）に並んでいる。横向きで読んでいるときも
// ボタンの物理位置は変わらないので、ヒントは Portrait 座標の下端に描けば
// そのままボタンの真横に出る（文字は 90° 回った見え方になる。本家と同じ方式）。
// 縦持ち（通常・反転）では向きを変えない。反転時の「上端に描く」判定は
// 呼び出し側がそのまま行える。
class HintOrientationScope {
 public:
  explicit HintOrientationScope(GfxRenderer& renderer) : renderer(renderer), original(renderer.getOrientation()) {
    if (isLandscape(original)) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    }
  }
  ~HintOrientationScope() { renderer.setOrientation(original); }
  HintOrientationScope(const HintOrientationScope&) = delete;
  HintOrientationScope& operator=(const HintOrientationScope&) = delete;

  static bool isLandscape(const GfxRenderer::Orientation o) {
    return o == GfxRenderer::Orientation::LandscapeClockwise ||
           o == GfxRenderer::Orientation::LandscapeCounterClockwise;
  }

 private:
  GfxRenderer& renderer;
  const GfxRenderer::Orientation original;
};
