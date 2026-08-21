#pragma once

// Tag/version parsing and ordering for the OTA update channels. Header-only and
// free of any Arduino/ESP dependency on purpose, so the same code a device runs
// can be exercised by the host test in test/ota_channel/.

#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "../BuildInfo.h"

// A release's (or this build's) position in time, derived from its tag alone.
struct ReleaseVersionKey {
  bool valid = false;
  bool isDev = false;
  // Dev releases: the tag's "YYYYMMDD-HHMMSS" stamp.
  char devStamp[16] = {};
  // Release/RC tags: the semantic version, plus the RC ordinal. rc == 0 means a
  // plain release, which ranks above every RC of the same triple.
  int major = 0;
  int minor = 0;
  int patch = 0;
  int rc = 0;
  // The build sits past its nearest tag ("0.1.17-3-gabc", "0.1.17-dev-…"), i.e.
  // it is not any released version. Only ever set for this device.
  bool offTag = false;
};

namespace releaseVersion {

// Update channels, nested: each one also accepts everything the channels above
// it accept. Values are persisted as CrossPointSettings::otaChannel, so they
// must stay in sync with CrossPointSettings::OTA_CHANNEL.
enum Channel : uint8_t {
  CHANNEL_STABLE = 0,  // vX.Y.Z only
  CHANNEL_RC = 1,      // + vX.Y.Z-rcN
  CHANNEL_DEV = 2,     // + dev-YYYYMMDD-HHMMSS
};

inline bool isDigits(const char* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
  }
  return true;
}

// Longest digit run accepted per version segment. Nine digits always fit in an
// int, which keeps the result identical on the device (RISC-V ILP32, where
// LONG_MAX == INT_MAX and strtol saturates instead of reporting overflow) and on
// the 64-bit host the tests run on. Without the cap, "v99999999999.0.0" would be
// rejected by the host test but silently become 2147483647 on the device.
constexpr size_t kMaxSegmentDigits = 9;

// Parses "<num>.<num>.<num>" at p. Returns the position just past it, or
// nullptr if the text does not start with three dot-separated numbers.
inline const char* parseTriple(const char* p, int* major, int* minor, int* patch) {
  int* out[3] = {major, minor, patch};
  for (int i = 0; i < 3; i++) {
    if (*p < '0' || *p > '9') return nullptr;  // reject the signs and spaces strtol would accept
    size_t digits = 0;
    while (p[digits] >= '0' && p[digits] <= '9') digits++;
    if (digits > kMaxSegmentDigits) return nullptr;
    char* end = nullptr;
    const long v = strtol(p, &end, 10);
    if (end == p || v < 0) return nullptr;
    *out[i] = static_cast<int>(v);
    p = end;
    if (i < 2) {
      if (*p != '.') return nullptr;
      p++;
    }
  }
  return p;
}

// Anchored at both ends on purpose: only the two tag namespaces this repo
// publishes firmware under are recognised. Anything else (sd-fonts, a future
// "v0.2.0-beta1") is rejected rather than guessed at.
inline bool parseCandidateTag(const char* tag, ReleaseVersionKey* out) {
  *out = ReleaseVersionKey{};
  if (!tag || tag[0] == '\0') return false;

  if (strncmp(tag, "dev-", 4) == 0) {
    const char* stamp = tag + 4;
    if (strlen(stamp) != CROSSPOINT_BUILD_TIME_LEN) return false;
    if (!isDigits(stamp, 8) || stamp[8] != '-' || !isDigits(stamp + 9, 6)) return false;
    memcpy(out->devStamp, stamp, CROSSPOINT_BUILD_TIME_LEN + 1);
    out->isDev = true;
    out->valid = true;
    return true;
  }

  if (tag[0] != 'v' && tag[0] != 'V') return false;
  const char* rest = parseTriple(tag + 1, &out->major, &out->minor, &out->patch);
  if (!rest) return false;
  if (*rest == '\0') {
    out->valid = true;  // plain release, rc stays 0
    return true;
  }
  if (strncmp(rest, "-rc", 3) != 0) return false;
  const char* digits = rest + 3;
  size_t rcDigits = 0;
  while (digits[rcDigits] >= '0' && digits[rcDigits] <= '9') rcDigits++;
  if (rcDigits == 0 || rcDigits > kMaxSegmentDigits) return false;
  char* end = nullptr;
  const long n = strtol(digits, &end, 10);
  if (*end != '\0' || n <= 0) return false;
  out->rc = static_cast<int>(n);
  out->valid = true;
  return true;
}

// Lenient by necessity: scripts/git_branch.py produces "0.1.17", "0.1.18-rc1",
// "0.1.17-3-gabc1234" (a Dev Build) and "0.1.17-dev-branch-abc1234" (a local
// build). Anything past the triple that is not an RC ordinal means the build
// sits ahead of its tag, which is what offTag records. RC and offTag coexist
// ("0.1.18-rc1-3-gabc").
inline bool parseDeviceVersion(const char* version, ReleaseVersionKey* out) {
  *out = ReleaseVersionKey{};
  if (!version || version[0] == '\0') return false;

  const char* p = (version[0] == 'v' || version[0] == 'V') ? version + 1 : version;
  const char* rest = parseTriple(p, &out->major, &out->minor, &out->patch);
  if (!rest) return false;
  out->valid = true;

  if (strncmp(rest, "-rc", 3) == 0) {
    const char* digits = rest + 3;
    size_t rcDigits = 0;
    while (digits[rcDigits] >= '0' && digits[rcDigits] <= '9') rcDigits++;
    if (rcDigits > 0 && rcDigits <= kMaxSegmentDigits) {
      char* end = nullptr;
      const long n = strtol(digits, &end, 10);
      if (n > 0) {
        out->rc = static_cast<int>(n);
        rest = end;
      }
    }
  }
  out->offTag = (*rest != '\0');
  return true;
}

// Compares two release-line keys. A plain release outranks every RC of the same
// triple; a higher RC ordinal outranks a lower one.
inline int cmpReleaseLine(const ReleaseVersionKey& a, const ReleaseVersionKey& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  const int ra = a.rc == 0 ? INT_MAX : a.rc;
  const int rb = b.rc == 0 ? INT_MAX : b.rc;
  if (ra != rb) return ra < rb ? -1 : 1;
  return 0;
}

// True when the candidate's tag namespace is one the channel delivers. The
// channels nest: DEV takes everything, RC takes releases and RCs, STABLE takes
// releases only.
inline bool isCandidateInChannel(const ReleaseVersionKey& candidate, Channel channel) {
  if (!candidate.valid) return false;
  if (candidate.isDev) return channel == CHANNEL_DEV;
  if (candidate.rc != 0) return channel >= CHANNEL_RC;
  return true;
}

// True when installing the candidate would move this device forward.
// deviceBuildTime is CROSSPOINT_BUILD_TIME, the running build's stamp.
inline bool isCandidateNewerThanDevice(const ReleaseVersionKey& candidate, const ReleaseVersionKey& device,
                                       const char* deviceBuildTime, Channel channel) {
  if (candidate.isDev) {
    // Both sides are fixed-width zero-padded UTC stamps, so strcmp is a
    // chronological comparison. Equality is this device's own Dev Build.
    return strcmp(candidate.devStamp, deviceBuildTime) > 0;
  }
  if (!device.valid) return false;  // nothing to compare against; stay put

  const int cmp = cmpReleaseLine(candidate, device);
  if (cmp > 0) return true;

  // Escape hatch. A build past its tag is not any released version, so the
  // release with the same triple is the route back onto the selected channel's
  // line — the only OTA way off a Dev Build while #86 leaves rollback disabled.
  // Suppressed on DEV, where such a device is already in-channel: allowing it
  // there would ping-pong between the release and the next Dev Build forever.
  return cmp == 0 && device.offTag && channel != CHANNEL_DEV;
}

// Ranks two candidates that both already qualify, so the scan can keep one
// winner while streaming.
inline bool candidateBeatsWinner(const ReleaseVersionKey& candidate, const ReleaseVersionKey& winner) {
  if (!winner.valid) return true;
  // Every v* tag is cut from the default branch (release-dispatch.yml refuses
  // any other), so the newest Dev Build is built from a commit that already
  // contains every released one. A qualifying Dev Build therefore outranks any
  // release candidate here.
  if (candidate.isDev != winner.isDev) return candidate.isDev;
  if (candidate.isDev) return strcmp(candidate.devStamp, winner.devStamp) > 0;
  return cmpReleaseLine(candidate, winner) > 0;
}

// The whole selection gate for one release, as applied while streaming: in
// channel, ahead of this device, and better than what has been picked so far.
inline bool shouldReplaceWinner(const ReleaseVersionKey& candidate, const ReleaseVersionKey& winner,
                                const ReleaseVersionKey& device, const char* deviceBuildTime, Channel channel) {
  return isCandidateInChannel(candidate, channel) &&
         isCandidateNewerThanDevice(candidate, device, deviceBuildTime, channel) &&
         candidateBeatsWinner(candidate, winner);
}

}  // namespace releaseVersion
