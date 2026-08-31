<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- Added the read-only FAP_SCREENSHOT_V1 serial protocol required by the
  community publisher: a dedicated task watches the USB-serial console for
  the `FAP_SCREENSHOT_V1` line and replies with a header plus the current
  screen rendered as a tight-packed RGB565 framebuffer (LVGL snapshot into a
  statically reserved full-screen buffer — the runtime heap is too fragmented
  for a 150 KB contiguous allocation — with logs muted during the binary
  window). The task installs the
  USB-serial-JTAG driver itself (the app boots no REPL, so nothing else
  does) and moves the console onto the driver path, matching the official
  REPL combination. The command is strictly observational — it never
  reboots, flashes, or touches settings or credentials.
- Smoothed the on-screen loudness readout: a model-layer asymmetric EMA
  (fast attack ~130 ms, slow release ~0.5 s, Q8 fixed-point, first-frame
  passthrough) replaces the raw per-frame value for the number, bar, and
  zone color, so the display settles instead of jumping every 80 ms. Peak
  and mean statistics and the alarm state machine intentionally keep raw
  values so transients and alarm response stay accurate, and session reset
  preserves the smoother so the readout no longer flashes 0.0 after an
  OK-key reset. Attack/release/convergence behavior is host-tested.
- Fixed meter-page layout and state issues found in a UI review: the 28 pt
  reading is right-aligned in a fixed-width box so >= 100 dB values no
  longer collide with the dB unit or the status badge; the "90" scale tick
  is right-aligned to the panel edge instead of overflowing it; the
  loudness-zone word (QUIET..EXTREME) is shown beside the threshold row
  and follows microphone errors; and the mascot now actually switches to
  its inverted alarm style on alarm edges (the edge-detect flag was
  overwritten before the style update ran, so the recolor never fired).
- Added an ambient sound meter application: the firmware boots directly into
  the meter page, replacing the BSP demo menu (unused demo pages are no longer
  built). Microphone RMS level is shown as an uncalibrated pseudo-SPL readout
  with a volume bar and threshold marker, a 40-100 dB adjustable alarm
  threshold with hysteresis and debounce, alarm recoloring of the whole screen,
  session peak/mean/duration/alarm-count statistics with OK-key reset, and an
  NVS-persisted threshold. Level math and the alarm state machine live in a
  host-testable `sound_meter_model` module wired into the static validation
  gate.
- Reworked the meter UI for the 240x320 screen: a 28pt zone-colored read
  (quiet/normal/moderate/loud/extreme), a five-color zone scale with 30/60/90
  dB ticks under the bar, a white peak-hold line and orange threshold marker on
  the bar, a status badge (green MONITOR / inverted ALARM / gray MIC FAIL),
  four-color statistic labels, and an inverted color scheme while alarming.
- Added automatic screen blanking: after three minutes without key activity in
  the monitoring state the backlight turns off while sampling and statistics
  keep running; an alarm threshold crossing or a sudden level jump (>= 20 dB
  from the slow EMA, two frames) wakes the screen, and any key wakes it
  immediately.
- Reworked key semantics: UP/DOWN now adjust backlight brightness (short press
  ±10%, long press ±25%, clamped to 10..100%, NVS-persisted, with a transient
  LIGHT popup), while the alarm threshold keeps its persisted value. Long-press
  OK blanks the screen immediately; the first key press after blanking only
  wakes the screen (no accidental brightness change or statistic reset), and
  wake-ups restore the user brightness instead of full brightness.
- Added a top-right battery icon driven by the CW2017 fuel gauge: the fill
  width tracks state of charge, colored green/yellow/red by level, and gray
  when the gauge is unavailable; the acquisition task polls it about every
  five seconds. Mapping helpers are host-tested.
- The mascot robot now reacts to loudness zones instead of only jumping on
  alarm edges: its antenna light, face, eyes, and mouth recolor per zone, the
  mouth widens as it gets louder, and it bobs faster and higher from moderate
  upward (alarm recolors it white/red). Zone-to-style mapping is host-tested.
- Fixed a boot white screen: the meter page (~90 objects, about 35 KB of LVGL
  allocations at peak) no longer fits the template's 24 KB LVGL built-in pool,
  so `lv_obj_create` returned NULL mid-build and the very next style call
  dereferenced it while the backlight was already on — no frame was ever
  rendered. LVGL now allocates from the system heap
  (`CONFIG_LV_USE_CLIB_MALLOC`): the 24 KB static reservation is reclaimed
  and the page still leaves well over 100 KB of heap free; entering the page
  logs the post-build free heap for field diagnosis. Root cause and fix are
  verified by a host harness that replicates the build sequence with a 24 KB
  pool (exhausted at object 37) and the full animated runtime (no crash,
  ~35 KB peak). Defensive hardening in the same class: `app_main` builds the
  whole page and the esp_timer task runs the key callbacks (which create the
  brightness panel on demand), both on the default 3584-byte stacks; both are
  raised to 8 KB, well covered by the reclaimed static DRAM.
- Established a validation convention for UI math: color/geometry mappings
  that must match between firmware and host tests live in `ui_pixel_math`
  (battery level/fill, mascot zone styles) and are asserted in
  `test_ui_pixel_math.c`.
- Made mini-program BLE install compatibility a template-level invariant: fixed
  protected `cardid`/Recovery partitions, retained the five-second UP-key
  Recovery boot hook, and added CI validation for merged-image structure,
  partition MD5/ranges, the 3 MB app limit, and protected payload exclusion.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
