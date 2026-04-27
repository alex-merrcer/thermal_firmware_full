const { callIotBridge } = require("../../utils/cloud");
const {
  buildThermalStatus,
  formatDateTime,
  formatRelativeUpdate,
  formatTemperature,
  getTemperatureUnitLabel,
  isUsableNumber,
  normalizeOnlineStatus,
  resolveFriendlyDeviceName,
} = require("../../utils/format");

function hasValidTime(value) {
  return isUsableNumber(value) && value > 0;
}

function buildMetricItems(rawData, unit, options) {
  const unitLabel = getTemperatureUnitLabel(unit);
  const hasSnapshot = !!(options && options.hasSnapshot);
  const isOffline = !!(options && options.isOffline);
  const stateText = !hasSnapshot ? "等待数据" : isOffline ? "最后快照" : "实时数据";

  return [
    {
      key: "maxTemp",
      label: "最高温",
      valueText: formatTemperature(rawData.maxTemp, unit),
      unitLabel,
      tone: "max",
      featured: true,
      stale: isOffline,
      stateText,
    },
    {
      key: "centerTemp",
      label: "中心温",
      valueText: formatTemperature(rawData.centerTemp, unit),
      unitLabel,
      tone: "center",
      featured: false,
      stale: isOffline,
      stateText,
    },
    {
      key: "minTemp",
      label: "最低温",
      valueText: formatTemperature(rawData.minTemp, unit),
      unitLabel,
      tone: "min",
      featured: false,
      stale: isOffline,
      stateText,
    },
  ];
}

function buildHeroFacts(rawData, online, hasSnapshot) {
  return [
    {
      key: "status",
      label: "设备状态",
      value: online.text,
    },
    {
      key: "latest",
      label: "最后上报",
      value: hasSnapshot ? formatRelativeUpdate(rawData.latestPropertyTimeMs) : "暂无数据",
    },
    {
      key: "snapshot",
      label: "快照时间",
      value: hasSnapshot ? formatDateTime(rawData.latestPropertyTimeMs) : "--",
    },
    {
      key: "cloud",
      label: "云端检查",
      value: hasValidTime(rawData.fetchedAtMs) ? formatRelativeUpdate(rawData.fetchedAtMs) : "--",
    },
  ];
}

function buildDashboardViewModel(rawData, settings) {
  const normalizedRaw = rawData || {};
  const normalizedSettings = settings || {
    temperatureUnit: "C",
    alarmThresholdC: 50,
  };
  const online = normalizeOnlineStatus(normalizedRaw.onlineStatus);
  const hasSnapshot = hasValidTime(normalizedRaw.latestPropertyTimeMs);
  const isOffline = !online.isOnline;
  const thermalStatus = buildThermalStatus(
    normalizedRaw.maxTemp,
    online.isOnline,
    normalizedSettings.alarmThresholdC
  );

  let freshnessTitle = "等待首条温度快照";
  let freshnessDescription = "设备联网并开始上报后，这里会显示最新的温度快照和状态判断。";
  let freshnessTone = "pending";

  if (hasSnapshot && online.isOnline) {
    freshnessTitle = "正在显示实时温度";
    freshnessDescription = `最新温度已在 ${formatRelativeUpdate(
      normalizedRaw.latestPropertyTimeMs
    )} 更新，首页会继续按你的刷新设置自动同步。`;
    freshnessTone = "normal";
  } else if (hasSnapshot) {
    freshnessTitle = "当前显示最后一次有效快照";
    freshnessDescription = `设备当前离线，以下温度来自 ${formatDateTime(
      normalizedRaw.latestPropertyTimeMs
    )} 的最后一次有效快照。`;
    freshnessTone = "offline";
  }

  return {
    displayName: resolveFriendlyDeviceName(normalizedRaw),
    online,
    hasSnapshot,
    latestUpdateText: formatDateTime(normalizedRaw.latestPropertyTimeMs),
    latestUpdateRelativeText: formatRelativeUpdate(normalizedRaw.latestPropertyTimeMs),
    fetchedAtText: formatDateTime(normalizedRaw.fetchedAtMs),
    thresholdText: `${Number(normalizedSettings.alarmThresholdC).toFixed(1)}°C`,
    metrics: buildMetricItems(normalizedRaw, normalizedSettings.temperatureUnit, {
      hasSnapshot,
      isOffline,
    }),
    heroFacts: buildHeroFacts(normalizedRaw, online, hasSnapshot),
    thermalStatus,
    freshnessTitle,
    freshnessDescription,
    freshnessTone,
    statusHintText: online.isOnline
      ? "按最高温和本地报警阈值实时判断"
      : "设备离线时，状态判断仅基于最后一次有效温度快照",
  };
}

Page({
  data: {
    loading: false,
    errorMessage: "",
    dashboard: buildDashboardViewModel(
      {},
      {
        temperatureUnit: "C",
        alarmThresholdC: 50,
      }
    ),
    metrics: buildMetricItems({}, "C", {
      hasSnapshot: false,
      isOffline: false,
    }),
  },

  onLoad() {
    this.bootstrap();
  },

  onShow() {
    const cached = getApp().globalData.lastDashboardData;

    this.syncSettings();
    this.startAutoRefreshIfNeeded();

    if (cached) {
      this.rawDashboardData = cached;
      this.applyDashboardData(cached);
    }
  },

  onHide() {
    this.stopAutoRefresh();
  },

  onUnload() {
    this.stopAutoRefresh();
  },

  onPullDownRefresh() {
    this.loadDashboard({
      fromPullDown: true,
    });
  },

  onRefreshTap() {
    this.loadDashboard();
  },

  bootstrap() {
    const app = getApp();
    const cached = app.globalData.lastDashboardData;

    this.syncSettings();

    if (cached) {
      this.rawDashboardData = cached;
      this.applyDashboardData(cached);
    }

    if (!app.globalData.cloudReady) {
      this.setData({
        errorMessage: "服务暂未准备好，请稍后再试。",
      });
      return;
    }

    this.loadDashboard({
      silent: !!cached,
    });
  },

  syncSettings() {
    const app = getApp();
    const settings = app.getSettings ? app.getSettings() : app.globalData.settings;

    this.settings = settings;

    if (this.rawDashboardData) {
      this.applyDashboardData(this.rawDashboardData);
      return;
    }

    const dashboard = buildDashboardViewModel({}, settings);

    this.setData({
      dashboard,
      metrics: dashboard.metrics,
    });
  },

  applyDashboardData(rawData) {
    const dashboard = buildDashboardViewModel(rawData, this.settings);

    this.setData({
      dashboard,
      metrics: dashboard.metrics,
    });
  },

  loadDashboard(options) {
    if (this.data.loading) {
      if (options && options.fromPullDown) {
        wx.stopPullDownRefresh();
      }
      return;
    }

    this.setData({
      loading: true,
      errorMessage: options && options.silent ? this.data.errorMessage : "",
    });

    callIotBridge("getDashboardData")
      .then((data) => {
        const app = getApp();

        app.globalData.lastDashboardData = data;
        this.rawDashboardData = data;
        this.applyDashboardData(data);
        this.setData({
          errorMessage: "",
        });
      })
      .catch((error) => {
        this.setData({
          errorMessage: error.message || "加载失败，请稍后重试。",
        });
      })
      .finally(() => {
        this.setData({
          loading: false,
        });

        if (options && options.fromPullDown) {
          wx.stopPullDownRefresh();
        }
      });
  },

  startAutoRefreshIfNeeded() {
    this.stopAutoRefresh();

    if (!this.settings || !this.settings.autoRefreshEnabled) {
      return;
    }

    this.autoRefreshTimer = setInterval(() => {
      if (!this.data.loading) {
        this.loadDashboard({
          silent: true,
        });
      }
    }, this.settings.autoRefreshIntervalSec * 1000);
  },

  stopAutoRefresh() {
    if (this.autoRefreshTimer) {
      clearInterval(this.autoRefreshTimer);
      this.autoRefreshTimer = null;
    }
  },

});
