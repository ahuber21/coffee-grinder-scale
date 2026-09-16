#pragma once

/**
 * Native simulator driver -- included (textually, not linked) from the
 * bottom of DisplayTask.cpp only when ARDUINO isn't defined, after that
 * file's anonymous namespace closes, so it can still call `renderMode()`
 * and reach `g_tft`/`g_lastConnColor` (anonymous-namespace names are
 * only link-restricted across translation units, not within one). Not
 * part of the real firmware in any way. See `tools/display_sim/README.md`.
 *
 * Scripts three scenarios (BOOT, a full IDLE->CONFIRM->TARE->GRINDING->
 * STOPPING->TOPUP->FINALIZE dose, and an OTA_UPDATE run) through the
 * exact same `renderMode()` the real Display task calls, stepping
 * simulated time in fine increments (finer than drawClumpField's own
 * ~70ms animation throttle, so that throttle actually governs frame
 * output the same way it would against a real ~60fps poll loop) and
 * capturing frames at a fixed output rate. Each scenario's captured
 * frames are dumped as a tiny custom binary (`.dsim`: a 16-byte header --
 * width/height/frame count/fps -- followed by raw RGB565 pixel data,
 * frames concatenated) for `tools/display_sim/dsim_to_gif.py` to turn
 * into something actually viewable.
 */

#include <cstdio>
#include <string>
#include <vector>

namespace sim {

struct FrameDump {
  std::vector<uint16_t> pixels;  // frames concatenated, kW*kH uint16 each
  uint32_t frameCount = 0;
};

void captureFrame(FrameDump &dump) {
  for (int y = 0; y < FakeCanvas::kHeight; ++y) {
    for (int x = 0; x < FakeCanvas::kWidth; ++x) {
      dump.pixels.push_back(g_tft.pixels[y][x]);
    }
  }
  dump.frameCount++;
}

void writeDump(const std::string &path, const FrameDump &dump, uint32_t fps) {
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::perror(path.c_str());
    return;
  }
  std::fwrite("DSIM", 1, 4, f);
  uint32_t header[4] = {static_cast<uint32_t>(FakeCanvas::kWidth),
                         static_cast<uint32_t>(FakeCanvas::kHeight), dump.frameCount, fps};
  std::fwrite(header, sizeof(uint32_t), 4, f);
  std::fwrite(dump.pixels.data(), sizeof(uint16_t), dump.pixels.size(), f);
  std::fclose(f);
  std::printf("wrote %s: %u frames at %dx%d, %ufps\n", path.c_str(), dump.frameCount,
              FakeCanvas::kWidth, FakeCanvas::kHeight, fps);
}

/** Mirrors displayTaskFn's applyCommand's mode-change clear -- kept in sync by hand. */
void applyModeChange(DisplayCommand &last, const DisplayCommand &cmd) {
  bool modeChanged = cmd.mode != last.mode;
  if (modeChanged) {
    g_tft.fillScreen(ST7735_BLACK);
    g_lastConnColor = 0;
  }
  renderMode(cmd, modeChanged);
  last = cmd;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

/**
 * Steps simulated time forward from `last`'s current state to `to` over
 * `durationMs`, in `kStepMs` increments -- interpolating current_grams/
 * elapsed_s linearly, holding every other field at `to`'s value -- and
 * captures a frame every `1000/kCaptureFps` ms of simulated time.
 */
void runSegment(FrameDump &dump, DisplayCommand &last, DisplayCommand to, uint32_t durationMs) {
  constexpr uint32_t kStepMs = 5;      // finer than drawClumpField's ~70ms tick
  constexpr uint32_t kCaptureFps = 24;
  constexpr uint32_t kCaptureEveryMs = 1000 / kCaptureFps;

  DisplayCommand from = last;
  uint32_t elapsed = 0;
  uint32_t sinceCapture = kCaptureEveryMs;  // force a capture on the segment's first sample
  while (elapsed <= durationMs) {
    float t = durationMs > 0 ? static_cast<float>(elapsed) / static_cast<float>(durationMs) : 1.0f;
    if (t > 1.0f) t = 1.0f;
    DisplayCommand cmd = to;
    cmd.current_grams = lerp(from.current_grams, to.current_grams, t);
    cmd.elapsed_s = lerp(from.elapsed_s, to.elapsed_s, t);

    if (elapsed > 0) simMillisRef() += kStepMs;
    applyModeChange(last, cmd);

    sinceCapture += kStepMs;
    if (sinceCapture >= kCaptureEveryMs) {
      captureFrame(dump);
      sinceCapture = 0;
    }
    elapsed += kStepMs;
  }
}

}  // namespace sim

int main(int argc, char **argv) {
  using sim::FrameDump;
  using sim::runSegment;
  using sim::writeDump;

  std::string outDir = argc > 1 ? argv[1] : ".";

  // Distinct from every real DisplayMode value, so the very first
  // segment's mode always registers as "changed" and gets its initial
  // full-force render -- same reason displayTaskFn seeds `last.mode =
  // DisplayMode::BOOT` and force-renders once before its own loop starts.
  DisplayCommand last{};
  last.mode = static_cast<DisplayMode>(255);

  FrameDump bootDump, doseDump, otaDump;

  {
    DisplayCommand boot{};
    boot.mode = DisplayMode::BOOT;
    runSegment(bootDump, last, boot, 2200);
  }
  writeDump(outDir + "/boot.dsim", bootDump, 24);

  {
    DisplayCommand idle{};
    idle.mode = DisplayMode::IDLE;
    runSegment(doseDump, last, idle, 800);

    DisplayCommand confirm{};
    confirm.mode = DisplayMode::CONFIRM;
    confirm.target_grams = 9.5f;
    runSegment(doseDump, last, confirm, 800);

    DisplayCommand tare{};
    tare.mode = DisplayMode::TARE;
    runSegment(doseDump, last, tare, 500);

    // GRINDING: main grind stops a bit short of the corrected target,
    // anticipating coast -- matches real behavior (see AR-072/AR-073).
    DisplayCommand grinding{};
    grinding.mode = DisplayMode::GRINDING;
    grinding.target_grams = 9.5f;
    grinding.current_grams = 8.8f;
    grinding.elapsed_s = 9.2f;
    runSegment(doseDump, last, grinding, 9200);

    DisplayCommand stopping{};
    stopping.mode = DisplayMode::STOPPING;
    stopping.target_grams = 9.5f;
    stopping.current_grams = 9.27f;  // coast lands it close to the corrected target
    stopping.elapsed_s = 10.7f;
    runSegment(doseDump, last, stopping, 1500);

    // TOPUP: one pulse (fast jump), then its settle wait.
    DisplayCommand pulseEnd{};
    pulseEnd.mode = DisplayMode::TOPUP;
    pulseEnd.target_grams = 9.5f;
    pulseEnd.current_grams = 9.48f;
    pulseEnd.elapsed_s = 11.0f;
    runSegment(doseDump, last, pulseEnd, 300);
    DisplayCommand settle = pulseEnd;
    settle.elapsed_s = 12.0f;
    runSegment(doseDump, last, settle, 1000);

    DisplayCommand finalize = settle;
    finalize.mode = DisplayMode::FINALIZE;
    runSegment(doseDump, last, finalize, 3000);
  }
  writeDump(outDir + "/dose.dsim", doseDump, 24);

  {
    DisplayCommand ota{};
    ota.mode = DisplayMode::OTA_UPDATE;
    ota.ota_percent = 0;
    runSegment(otaDump, last, ota, 100);
    for (int pct = 5; pct <= 100; pct += 5) {
      DisplayCommand step = ota;
      step.ota_percent = static_cast<uint8_t>(pct);
      runSegment(otaDump, last, step, 300);  // ~300ms/5% -- a realistic OTA cadence
    }
  }
  writeDump(outDir + "/ota.dsim", otaDump, 24);

  return 0;
}
