/**
 * ADD THIS TO backend/server.js in the "reading" section, after /api/display
 */

/**
 * Slim endpoint for the ESP32 LED wall. Returns only the data needed for the
 * 4 display pages (Portfolio, Events, Holdings, Offline), ~1-2KB JSON.
 *
 * Polled every 30 seconds from the ESP32 over WiFi, so payloads must be small
 * and structure must be flat/simple (no parsing complexity on the microcontroller).
 */
app.get("/api/matrix", async (_req, res) => {
  try {
    const now = new Date();
    const [items, money, brief] = await Promise.all([
      allItems(),
      getMeta("moneySummary", null),
      getMeta("lastBrief", null),
    ]);

    // Portfolio: total value, day change ($), day change (%)
    const portfolio = money ? {
      total: money.total || 0,
      dayChange: money.dayValue || 0,
      dayChangePercent: money.dayPercent || 0,
    } : null;

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
          dayChangePercent: p.dayPercent || 0,
          weightPercent: Math.round(((p.value || 0) / (money.total || 1)) * 1000) / 10,
        }))
      : [];

    // Last refresh timestamp (for offline detection on ESP32)
    const lastRefresh = money?.at || null;

    res.json({
      timestamp: now.getTime(),
      lastRefresh,
      portfolio,
      events: todayEvents,
      dailyBusyPercent,
      holdings,
    });
  } catch (err) {
    log.error(err.message);
    res.status(500).json({ error: err.message });
  }
});
