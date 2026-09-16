// WakeupTypes.h — round 91 compile-fix follow-up.
//
// Why this exists as a separate file instead of just living in the .ino:
// moving struct RGBf further UP inside esp32-led-wall.ino (next to the
// other small helpers, ahead of renderWakeUp()) did NOT fix the
// "'RGBf' does not name a type" error — it just moved which line the same
// error pointed at. Arduino's auto-prototype generator hoists a prototype
// for every function in the .ino to one fixed insertion point near the
// very top of the file (right after the leading block of comments/blank
// lines/#include directives), no matter where in the .ino the function or
// its return/parameter types are actually defined. A struct defined
// anywhere in the .ino's own body — even "near the top" — is still after
// that insertion point.
//
// The one thing that IS guaranteed to be visible by then is a header
// that's #include'd up in that same leading block, since the C
// preprocessor expands #include textually before Arduino's generated
// prototypes are even inserted. Auto-prototype generation also only
// applies to .ino content — a .h tab like this one is compiled as
// ordinary C++, never scanned for auto-prototyping — so RGBf/WakeupStar
// living here sidesteps the whole mechanism instead of fighting it.
//
// The helper FUNCTIONS that use these types (rgbf, lerp3f, packRGBf,
// hsvToRGBf, wakeupStarTwinkle, printBold) stay put in the .ino's "Small
// helpers" section — only the type definitions needed to move.
#pragma once

struct RGBf { float r, g, b; };

// Fixed points (not random) — same 9 the Twin uses, kept out of the sun's
// bottom-right landing spot and the top-left corner label.
struct WakeupStar { int x, y; unsigned long offset, cycle; };
