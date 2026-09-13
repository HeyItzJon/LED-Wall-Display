/**
 * This is a REFERENCE COPY of the /api/matrix route that actually lives in
 * pi-secretary/backend/server.js (in the "reading" section, after
 * /api/display). If you ever need to restore or diff it, this is what
 * should be there — but server.js is the source of truth; edit it there,
 * then update this file to match, not the other way around.
 */

/**
 * Slim endpoint for the ESP32 LED wall. Returns everything the firmware's
 * pages need — ticker (markets + top movers), today's events, top holdings —
 * ~2-3KB JSON. Polled every 30 seconds from the ESP32 over WiFi.
 *
 * IMPORTANT: this endpoint makes ZERO external API calls of its own. Every
 * field below is read straight out of meta blobs that the regular 15-minute
 * pull cycle (runSources → collectMoney / collectMarketNews) already wrote:
 *   - moneySummary.positions[].dayChangePct → gainers/losers
 *   - marketPulse.indices[].pct             → TSX/NASDAQ/S&P ticker line
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
    const shortLabel = (label) => (label === "S&P 500" ? "S&P" : label.toUpperCase());
    const markets = (marketPulse?.indices || [])
      .filter((i) => i.pct != null)
      .map((i) => ({
        symbol: shortLabel(i.label),
        changePercent: Math.round(i.pct * 100) / 100,
      }));

    // Top 3 gainers / losers by TODAY's move, from the positions the money
    // source already priced this pull — same dayChangePct the Finances page
    // shows on each holding row.
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
      .map((e) => ({
        time: e.clockTime || e.dueAt?.slice(11, 16) || "",
        title: (e.title || "").slice(0, 30), // truncate for display
        busyLevel: e.meta?.busyLevel || "medium", // "busy" | "medium" | "light"
      }))
      .sort((a, b) => a.time.localeCompare(b.time));

    // Daily busy score (0-100)
    const dailyBusyPercent = brief?.insights?.busyPercent || 0;

    // Top holdings (top 5 by value)
    const holdings = money?.positions
      ? money.positions.slice(0, 5).map((p) => ({
          symbol: p.ticker.replace(/\.(TO|V|NE|CN)$/i, ""),
          value: Math.round(p.value || 0),
          dayChangePercent: p.dayChangePct || 0,
          weightPercent: Math.round((p.weightPct || 0) * 10) / 10,
        }))
      : [];

    res.json({
      timestamp: now.getTime(),
      lastRefresh: money?.at || null,
      portfolio,
      markets, // TSX, NASDAQ, S&P with % change
      gainers, // Top 3 holdings up today
      losers, // Top 3 holdings down today
      events: todayEvents,
      dailyBusyPercent,
      holdings,
    });
  } catch (err) {
    log.error(err.message);
    res.status(500).json({ error: err.message });
  }
});
