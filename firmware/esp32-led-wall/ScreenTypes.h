// ScreenTypes.h — compile-fix companion to WakeupTypes.h.
//
// Same exact problem, different structs: Arduino's auto-prototype
// generator hoists a forward declaration for every function in
// esp32-led-wall.ino to one fixed point near the very top of the file
// (right after the leading block of comments/#include directives),
// regardless of where in the .ino the function — or a struct type it
// takes as a parameter or return value — is actually defined. The .ino's
// own "Struct types used across screens" comment block documented this
// same class of error and tried to fix it by moving these structs "up
// here, before any function definitions" within the .ino itself — but per
// WakeupTypes.h's own comment (which this file copies the reasoning from
// verbatim), that fix doesn't actually work: a struct defined anywhere in
// the .ino's own body, even at the very top, still sits AFTER the point
// where the hoisted prototypes land. Only a separate header, #include'd up
// in that same leading block, is guaranteed visible in time — the C
// preprocessor expands #include textually before Arduino's generated
// prototypes are even inserted, and auto-prototyping only ever scans .ino
// content, never a .h tab like this one.
//
// Every struct here is used as a function parameter or return type
// somewhere in the .ino (that's exactly what makes it subject to this
// bug) — see EventItem/newSpark/renderSparks/computeEventPlan/
// computeAllDayPlan/buildHoldingsStrip/drawStripCmd/buildMarketsStrip/
// buildNotifStrip/drawNotifCmd/alertLevelFor. Ball and BootTiming stayed
// behind in the .ino's own "Small helpers" section — they're only ever
// used as global/local variable types, never in a function signature, so
// they were never actually subject to this bug in the first place.
#pragma once

#include <Arduino.h>

struct EventItem {
  String time;
  String title;
  String busyLevel;  // "busy" | "medium" | "light" — always sent today
  String cal;        // "work"/"school"/"personal"/"important"/"cannotmiss"/"tests" — NOT sent yet, see handoff doc
  String desc;        // NOT sent yet, see handoff doc
  int    dur;          // minutes — NOT sent yet, defaults to 30
};

struct Spark { float x, y, life, maxLife, peak; };
struct ExcludeRect { int x0, x1, y0, y1; };
struct EventPlan { String cap, desc; int capOverflow, descOverflow; unsigned long dur; };
struct AllDayPlan { String cap; int capOverflow; unsigned long dur; };
struct StripCmd { bool isIcon; int x, y, w, h; String text; uint32_t iconCp; uint16_t color; };
struct NotifCmd { bool isIcon; int x, w; String text; uint32_t cp; };
struct AlertLevel { uint8_t ar, ag, ab, br, bg, bb, tr, tg, tb; };
