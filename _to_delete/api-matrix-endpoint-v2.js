/**
 * GET /api/matrix
 *
 * LED Wall Display Data Endpoint
 * Returns portfolio, market indices, and top movers for scrolling ticker display
 *
 * Response: ~2-3KB JSON payload
 * Refresh: Every 30 seconds (ESP32 polls)
 */

app.get("/api/matrix", async (req, res) => {
  try {
    const moneySummary = await getMoneySummary();

    // Helper: Extract price change from holding
    const calculateChange = (holding) => {
      const change = holding.quantity * (holding.currentPrice - holding.avgCost);
      const percent = ((holding.currentPrice - holding.avgCost) / holding.avgCost) * 100;
      return { change, percent };
    };

    // Get top 3 gainers from portfolio holdings
    const gainers = moneySummary.positions
      .map((pos) => {
        const { change, percent } = calculateChange(pos);
        return {
          symbol: pos.displaySymbol,
          changePercent: parseFloat(percent.toFixed(2)),
          change: parseFloat(change.toFixed(2)),
        };
      })
      .sort((a, b) => b.changePercent - a.changePercent)
      .slice(0, 3);

    // Get top 3 losers from portfolio holdings
    const losers = moneySummary.positions
      .map((pos) => {
        const { change, percent } = calculateChange(pos);
        return {
          symbol: pos.displaySymbol,
          changePercent: parseFloat(percent.toFixed(2)),
          change: parseFloat(change.toFixed(2)),
        };
      })
      .sort((a, b) => a.changePercent - b.changePercent)
      .slice(0, 3);

    // Fetch market indices (TSX, NASDAQ, NYSE)
    let markets = [];
    try {
      const { quotes } = require("yahoo-finance2").default;
      const indices = await quotes({
        symbols: ["^GSPTSE", "^IXIC", "^GSPC"],
        fields: ["symbol", "regularMarketChangePercent"],
      });

      markets = [
        {
          symbol: "TSX",
          ticker: "^GSPTSE",
          changePercent: parseFloat(
            (indices["^GSPTSE"]?.regularMarketChangePercent || 0).toFixed(2)
          ),
        },
        {
          symbol: "NASDAQ",
          ticker: "^IXIC",
          changePercent: parseFloat(
            (indices["^IXIC"]?.regularMarketChangePercent || 0).toFixed(2)
          ),
        },
        {
          symbol: "NYSE",
          ticker: "^GSPC",
          changePercent: parseFloat(
            (indices["^GSPC"]?.regularMarketChangePercent || 0).toFixed(2)
          ),
        },
      ];
    } catch (err) {
      console.error("Failed to fetch market indices:", err.message);
      // Fallback: empty markets array, display will continue with holdings only
      markets = [];
    }

    // Portfolio summary
    const portfolio = {
      total: parseFloat(moneySummary.grandTotal.toFixed(2)),
      dayChange: parseFloat(moneySummary.dailyAbsoluteChange.toFixed(2)),
      dayChangePercent: parseFloat(moneySummary.dailyPercentChange.toFixed(2)),
    };

    const response = {
      timestamp: Date.now(),
      lastRefresh: Date.now(),
      portfolio,
      markets, // TSX, NASDAQ, NYSE with % change
      gainers, // Top 3 holdings up
      losers, // Top 3 holdings down
    };

    res.json(response);
  } catch (error) {
    console.error("Error in /api/matrix:", error);
    res.status(500).json({
      error: "Failed to fetch matrix data",
      timestamp: Date.now(),
    });
  }
});
