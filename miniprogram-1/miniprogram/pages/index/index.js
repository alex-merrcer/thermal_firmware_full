const { callIotBridge } = require("../../utils/cloud");
const {
  buildThermalStatus,
  formatDateTime,
  formatRelativeUpdate,
  formatTemperature,
  getTemperatureUnitLabel,
  normalizeOnlineStatus,
} = require("../../utils/format");

function buildMetricItems(rawData, unit) {
  const unitLabel = getTemperatureUnitLabel(unit);

  return [
    {
      key: "maxTemp",
      label: "最高温",
      valueText: formatTemperature(rawData.maxTemp, unit),
      unitLabel,
      tone: "max",
      featured: true,
    },
    {
      key: "centerTemp",
      label: "中心温",
      valueText: formatTemperature(rawData.centerTemp, unit),
      unitLabel,
      tone: "center",
      featured: false,
    },
    {
      key: "minTemp",
      label: "最低温",
      valueText: formatTemperature(rawData.minTemp, unit),
      unitLabel,
      tone: "min",
      featured: false,
    },
  ];
}

function buildDashboardViewModel(rawData, settings, modelText) {
  const normalizedRaw = rawData || {};
  const normalizedSettings = settings || {
    temperatureUnit: "C",
    alarmThresholdC: 50,
  };
  const online = normalizeOnlineStatus(normalizedRaw.onlineStatus);
  const thermalStatus = buildThermalStatus(
    normalizedRaw.maxTemp,
    online.isOnline,
    normalizedSettings.alarmThresholdC
  );

  return {
    displayName: normalizedRaw.displayName || normalizedRaw.deviceName || "未命名设备",
    modelText: modelText || "红外热成像温度监测设备",
    online,
    latestUpdateText: formatDateTime(normalizedRaw.latestPropertyTimeMs),
    latestUpdateRelativeText: formatRelativeUpdate(normalizedRaw.latestPropertyTimeMs),
    fetchedAtText: formatDateTime(normalizedRaw.fetchedAtMs),
    thresholdText: `${Number(normalizedSettings.alarmThresholdC).toFixed(1)}°C`,
    metrics: buildMetricItems(normalizedRaw, normalizedSettings.temperatureUnit),
    thermalStatus,
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
    metrics: buildMetricItems({}, "C"),
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
        errorMessage: "云开发环境尚未配置完成，请先检查 app.js 中的 envId。",
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

    const dashboard = buildDashboardViewModel({}, settings, app.globalData.deviceModelText);

    this.setData({
      dashboard,
      metrics: dashboard.metrics,
    });
  },

  applyDashboardData(rawData) {
    const app = getApp();
    const dashboard = buildDashboardViewModel(rawData, this.settings, app.globalData.deviceModelText);

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

  goHistory() {
    wx.switchTab({
      url: "/pages/history/index",
    });
  },

  goSettings() {
    wx.switchTab({
      url: "/pages/settings/index",
    });
  },
});
