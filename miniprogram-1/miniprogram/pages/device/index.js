const { callIotBridge } = require("../../utils/cloud");
const {
  formatDateTime,
  formatRelativeUpdate,
  isUsableNumber,
  normalizeOnlineStatus,
  resolveFriendlyDeviceName,
} = require("../../utils/format");

function hasValidTime(value) {
  return isUsableNumber(value) && value > 0;
}

function buildDeviceViewModel(rawData) {
  const normalizedRaw = rawData || {};
  const online = normalizeOnlineStatus(normalizedRaw.onlineStatus);
  const hasSnapshot = hasValidTime(normalizedRaw.latestPropertyTimeMs);

  return {
    displayName: resolveFriendlyDeviceName(normalizedRaw),
    online,
    latestUpdateText: formatDateTime(normalizedRaw.latestPropertyTimeMs),
    latestUpdateRelativeText: formatRelativeUpdate(normalizedRaw.latestPropertyTimeMs),
    fetchedAtText: formatDateTime(normalizedRaw.fetchedAtMs),
    snapshotStateText: !hasSnapshot
      ? "暂无温度快照"
      : online.isOnline
      ? "温度数据正在实时更新"
      : "当前显示最后一次有效温度快照",
    snapshotDescription: !hasSnapshot
      ? "设备联网并开始上报后，这里会显示最近一次温度快照。"
      : online.isOnline
      ? "设备在线，可通过首页和历史页查看最新温度趋势。"
      : `设备离线，温度数据停留在 ${formatRelativeUpdate(normalizedRaw.latestPropertyTimeMs)}。`,
  };
}

Page({
  data: {
    loading: false,
    errorMessage: "",
    device: buildDeviceViewModel({}),
  },

  onLoad() {
    const app = getApp();

    if (app.globalData.lastDashboardData) {
      this.rawDashboardData = app.globalData.lastDashboardData;
      this.applyDashboardData(this.rawDashboardData);
    }
  },

  onShow() {
    const cached = getApp().globalData.lastDashboardData;

    if (cached) {
      this.rawDashboardData = cached;
      this.applyDashboardData(cached);
    }

    this.loadDeviceInfo({
      silent: !!this.rawDashboardData,
    });
  },

  onPullDownRefresh() {
    this.loadDeviceInfo({
      fromPullDown: true,
    });
  },

  onRefreshTap() {
    this.loadDeviceInfo();
  },

  applyDashboardData(rawData) {
    this.setData({
      device: buildDeviceViewModel(rawData),
    });
  },

  loadDeviceInfo(options) {
    const app = getApp();

    if (!app.globalData.cloudReady) {
      this.setData({
        errorMessage: "服务暂未准备好，请稍后再试。",
      });

      if (options && options.fromPullDown) {
        wx.stopPullDownRefresh();
      }
      return;
    }

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
        app.globalData.lastDashboardData = data;
        this.rawDashboardData = data;
        this.applyDashboardData(data);
      })
      .catch((error) => {
        this.setData({
          errorMessage: error.message || "设备信息加载失败，请稍后重试。",
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
});
