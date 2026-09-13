/**
 * This is a REFERENCE COPY of the /api/matrix route that actually lives in
 * pi-secretary/backend/server.js (in the "reading" section, after
 * /api/display). If you ever need to restore or diff it, this is what
 * should be there — but server.js is the source of truth; edit it there,
 * then update this file to match, not the other way around.
 *
 * Synced round 72 (2026-09-13) to add events[].cal/.dur/.desc and
 * dayOverview.hoursBusy/.hoursFree — see docs/JSON-CONTRACT.md in this repo
 * for what the firmware actually does with every field below, and for which
 * fields (markets/gainers/losers) are legacy, still sent, but read by
 * nothing — leftover from the pre-round-49 ticker design this firmware
 * replaced.
 */

/**
 * Slim endpoint for the ESP32 LED wall's screen-rotation firmware — returns
 * portfolio, today's events (with real calendar color/duration/description
 * as of round 72), a day-overview busy/free summary, top holdings, news
 * headlines, and market/mover data the current firmware doesn't read yet.
 * ~2-3KB JSON. Polled every 30 seconds from the ESP32 over WiFi
 * (`/api/matrix/command` is the separate, faster-polled live-control
 * channel — see lib/matrixControl.js, not reference-copied here).
 *
 * IMPORTANT: this endpoint makes ZERO external API calls of its own. Every
 * field below is read straight out of meta blobs that the regular 15-minute
 * pull cycle (runSources → collectMoney / collectMarketNews) already wrote,
 * or computed from `allItems()` (the same store every other page reads):
 *   - moneySummary.positions[].dayChangePct → gainers/losers (legacy, unused)
 *   - marketPulse.indices[].pct             → markets (legacy, unused)
 *   - marketPulse.headlines                 → news
 *   - allItems() calendar-source items      → events[], dayOverview
 * If you want fresher numbers, raise config.schedule.pullEveryMinutes rather
 * than adding a fetch here — this route just reads what's already cached.
 */
app.get("/api/matrix", async (_req, res) => {
  try {
    const now = new Date();
    const [items, money, marketPulse, brief] = await Promise.all([
      allItems(),
      getMeta("moneySummary", null),
      getMeta("marketPulse", null),
      getMeta("lastBrief", null),
    ]);

    // Portfolio: total value, day change ($), day change (%)
    const portfolio = money
      ? {
          total: Math.round((money.total || 0) * 100) / 100,
          dayChange: Math.round((money.dayChangeValue || 0) * 100) / 100,
          dayChangePercent: Math.round((money.dayPct || 0) * 100) / 100,
        }
      : null;

    // Market indices (TSX / NASDAQ / S&P) — from marketPulse, refreshed by
    // sources/marketNews.js on the same 15-minute cycle. No fetch here.
    // Legacy: the current screen-rotation firmware's Markets screen doesn't
    // read this at all yet (still a "COMING SOON" placeholder) — kept
    // sending since it's free and a future firmware update may use it.
    const shortLabel = (label) => (label === "S&P 500" ? "S&P" : label.toUpperCase());
    const markets = (marketPulse?.indices || [])
      .filter((i) => i.pct != null)
      .map((i) => ({
        symbol: shortLabel(i.label),
        changePercent: Math.round(i.pct * 100) / 100,
      }));

    // Top 3 gainers / losers by TODAY's move, from the positions the money
    // source already priced this pull — same dayChangePct the Finances page
    // shows on each holding row. Legacy, same as markets above: the current
    // firmware's Holdings screen does its own top-3-up/top-3-down selection
    // straight from `holdings[]` below, not from these.
    const movers = (money?.positions || [])
      .filter((p) => p.dayChangePct != null)
      .map((p) => ({
        symbol: p.ticker.replace(/\.(TO|V|NE|CN)$/i, ""),
        changePercent: Math.round(p.dayChangePct * 100) / 100,
      }));
    const gainers = [...movers].sort((a, b) => b.changePercent - a.changePercent).slice(0, 3);
    const losers = [...movers].sort((a, b) => a.changePercent - b.changePercent).slice(0, 3);

    // Events: today's events only, with busy level
    const today = new Intl.DateTimeFormat("en-CA", {
      timeZone: config.timezone,
      year: "numeric",
      month: "2-digit",
      day: "2-digit",
    }).format(now);

    const todayEvents = items
      .filter((i) => i.source === "calendar" && i.dueAt?.startsWith(today) && i.status === "open")
      .map((e) => {
        // dur: real end-minus-start minutes when we know the end time —
        // skip all-day events, there's no meaningful timeline-sliver width
        // for those. Clamped to a sane 5-600min range so a bad/missing end
        // time can't paint a degenerate or day-spanning bar. desc: the same
        // concise note/time/location/attendee one-liner already built for
        // the Tasks/Day list rows (e.detail from sources/calendar.js),
        // truncated to fit the wall's second scrolling line. cal: the real
        // calendar bucket, already computed at ingestion (sources/
        // calendar.js's calendarSwatch() call) — round 72, closes the
        // round-62 gap (wrong/fallback event colors on the wall).
        const hasRealDuration = e.meta?.end && !e.meta?.allDay;
        const dur = hasRealDuration
          ? Math.min(600, Math.max(5, Math.round((new Date(e.meta.end) - new Date(e.dueAt)) / 60000)))
          : 30;
        return {
          time: e.clockTime || e.dueAt?.slice(11, 16) || "",
          title: (e.title || "").slice(0, 30), // truncate for display
          busyLevel: e.meta?.busyLevel || "medium", // "busy" | "medium" | "light"
          cal: e.swatch || "",
          dur,
          desc: (e.detail || "").slice(0, 60),
        };
      })
      .sort((a, b) => a.time.localeCompare(b.time));

    // Daily busy score (0-100)
    const dailyBusyPercent = brief?.insights?.busyPercent || 0;

    // Day Overview's hoursBusy/hoursFree — round 72, closes the round-64
    // punch-list gap. Same weekForecast() math the Week page already uses
    // (see brief/display.js's buildDayContext for the identical filterLive
    // -> calendar-only -> weekForecast pattern), just asked for a single
    // day (today) instead of the 7-day window. commuteMin is deliberately
    // NOT included here — no real ETA source wired up yet (Jon: "I haven't
    // figured out the logic for that"); the Commuting screen keeps
    // degrading gracefully to "COMMUTE ETA COMING SOON" until it exists.
    const liveItems = filterLive(items, now);
    const calendarEvents = liveItems
      .filter((i) => i.source === "calendar" && i.dueAt && i.kind !== "system")
      .sort((a, b) => new Date(a.dueAt) - new Date(b.dueAt));
    const todayForecast = weekForecast(calendarEvents, liveItems.filter(isTaskLike), {
      now,
      tz: config.timezone,
      days: 1,
    }).days[0];
    const dayOverview = {
      hoursBusy: todayForecast?.busyHours ?? 0,
      hoursFree: todayForecast?.freeHours ?? 0,
    };

    // Top holdings (top 5 by value) — same shape the Holdings page already uses
    const holdings = money?.positions
      ? money.positions.slice(0, 5).map((p) => ({
          symbol: p.ticker.replace(/\.(TO|V|NE|CN)$/i, ""),
          value: Math.round(p.value || 0),
          dayChangePercent: p.dayChangePct || 0,
          weightPercent: Math.round((p.weightPct || 0) * 10) / 10,
        }))
      : [];

    // Headlines for a "News" screen — marketPulse is already being fetched
    // above for the ticker, so this is free: no new source, no new call.
    // Same truncate-for-display convention as todayEvents' title above.
    const news = (marketPulse?.headlines || []).slice(0, 6).map((h) => ({
      title: (h.title || "").slice(0, 60),
      source: h.source || null,
    }));

    // Whether anything actually traded today, per the Round 49 weekend-
    // color fix (sources/money.js's marketOpen gate) — free to include here
    // since `money` is already fetched above. Lets the firmware show an
    // honest "Markets are closed" screen on a weekend/holiday instead of a
    // stale weekday portfolio number presented as current.
    const marketOpen = money?.marketStatus != null;

    res.json({
      timestamp: now.getTime(),
      lastRefresh: money?.at || null,
      portfolio,
      markets, // TSX, NASDAQ, S&P with % change — legacy, unused by firmware
      gainers, // Top 3 holdings up today — legacy, unused by firmware
      losers, // Top 3 holdings down today — legacy, unused by firmware
      events: todayEvents,
      dailyBusyPercent,
      dayOverview,
      holdings,
      news,
      marketOpen,
    });
  } catch (err) {
    log.error(err.message);
    res.status(500).json({ error: err.message });
  }
});
