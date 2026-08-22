#pragma once

// Automatic recovery from a firmware that cannot boot.
//
// The bootloader supports it (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) but the
// Arduino core cancels it inside initArduino(), long before setup() runs — so a
// build that dies while loading fonts or parsing app state used to be marked
// good and stick, leaving an SD-card reflash as the only way out.
//
// This module defers that decision until setup() has finished. Reaching the end
// of setup() means fonts loaded, settings and app state parsed, and an activity
// started; anything that fails after that still leaves a device that boots and
// draws. A build that dies earlier never records the confirmation, so the next
// boot falls back to the previous app.
//
// Note this only helps when the other OTA slot holds a valid image. With an
// empty slot the bootloader re-picks the aborted app (it scans for a loadable
// image, not for the ota_state), which is a boot loop — the same behaviour as
// before this module existed, not a regression.
namespace ota_rollback {

// Confirms the running app so the bootloader keeps it. Only writes when the app
// is actually awaiting confirmation, so the common boot costs no flash write.
// Safe to call more than once.
void markCurrentAppValid();

// True when the previous boot failed to confirm itself and the bootloader fell
// back to this app. Detected from the other slot being ESP_OTA_IMG_ABORTED.
bool didRollBack();

// "NEW" / "PENDING_VERIFY" / "VALID" / "ABORTED" / "UNDEFINED" / "?" for the
// running partition. For the boot diagnostics; never null.
const char* runningStateName();

// Label of the running app partition ("app0" / "app1"). Never null.
const char* runningPartitionLabel();

}  // namespace ota_rollback
