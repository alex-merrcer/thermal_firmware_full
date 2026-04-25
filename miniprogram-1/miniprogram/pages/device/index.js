const { callIotBridge } = require("../../utils/cloud");
const {
  formatDateTime,
  formatRelativeUpdate,
  normalizeOnlineStatus,
} = require("../../utils/format");

const PENDING_CAPABILITIES = [
  {
    key: "wifi",
    title: "Wi-Fi 状态",
    description: "当前 firmware / cloud 链路未接入上云展示。",
  },
  {
    key: "ble",
    title: "蓝牙状态",
    description: "当前 firmware / cloud 链路未接入上云展示。",
  },
  {
    key: "ota",
    title: "OTA 更新",
    description: "当前 firmware / cloud 链路未接入上云展示。",
  },
  {
    key: "firmware",
    title: "固件版本",
    description: "当前 firmware / cloud 链路未接入上云展示。",
  },
];

function buildDeviceViewModel(rawData, modelText) {
  const normalizedRaw = rawData || {};
  const online = normalizeOnlineStatus(normalizedRaw.onlineStatus);

  return {
    displayName: normalizedRaw.displayName || normalizedRaw.deviceName || "未命名设备",
    online,
    modelText: modelText || "STM32 + ESP32 红外热成像测温系统",
    latestUpdateText: formatDateTime(normalizedRaw.latestPropertyTimeMs),
    latestUpdateRelativeText: formatRelativeUpdate(normalizedRaw.latestPropertyTimeMs),
    fetchedAtText: formatDateTime(normalizedRaw.fetchedAtMs),
  };
}

Page({
  data: {
    loading: false,
    errorMessage: "",
    device: buildDeviceViewModel({}),
    pendingCapabilities: PENDING_CAPABILITIES,
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
    const app = getApp();

    this.setData({
      device: buildDeviceViewModel(rawData, app.globalData.deviceModelText),
    });
  },

  loadDeviceInfo(options) {
    const app = getApp();

    if (!app.globalData.cloudReady) {
      this.setData({
        errorMessage: "云开发环境尚未配置完成，请先检查 app.js 中的 envId。",
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
