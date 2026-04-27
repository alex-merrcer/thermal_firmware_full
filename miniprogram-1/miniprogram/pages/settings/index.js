const { callIotBridge } = require("../../utils/cloud");
const { normalizeOnlineStatus } = require("../../utils/format");
const {
  ALLOWED_INTERVALS,
  DEFAULT_SETTINGS,
  normalizeThreshold,
} = require("../../utils/settings");

const UNIT_OPTIONS = [
  { label: "摄氏 °C", value: "C" },
  { label: "华氏 °F", value: "F" },
];

const PLACEHOLDER_ITEMS = [
  {
    key: "weather",
    title: "天气能力",
    description: "后续会和 ESP32 阶段 7 一起接入，当前小程序先不展示天气实况。",
  },
  {
    key: "firmwareUpdate",
    title: "固件更新详情",
    description: "OTA 状态已经开始上云，后续会补完整的小程序升级页与版本详情。",
  },
];

function formatThresholdInput(value) {
  return Number(value).toFixed(1);
}

Page({
  data: {
    errorMessage: "",
    loadingDevice: false,
    savingNickname: false,
    unitOptions: UNIT_OPTIONS.map((item) => item.label),
    intervalOptions: ALLOWED_INTERVALS.map((item) => `${item} 秒`),
    unitIndex: 0,
    intervalIndex: ALLOWED_INTERVALS.indexOf(DEFAULT_SETTINGS.autoRefreshIntervalSec),
    currentUnitLabel: UNIT_OPTIONS[0].label,
    currentIntervalLabel: `${DEFAULT_SETTINGS.autoRefreshIntervalSec} 秒`,
    autoRefreshEnabled: DEFAULT_SETTINGS.autoRefreshEnabled,
    thresholdInput: formatThresholdInput(DEFAULT_SETTINGS.alarmThresholdC),
    nicknameInput: "",
    deviceDisplayName: "未命名设备",
    onlineStatus: {
      text: "状态未知",
      tone: "pending",
    },
    placeholderItems: PLACEHOLDER_ITEMS,
    versionText: "v2.0.0",
    debugHintText: "连续点击版本区域 5 次进入隐藏调试页",
  },

  onLoad() {
    const app = getApp();

    this.setData({
      versionText: app.globalData.appVersion || "v2.0.0",
    });
    this.syncSettingsToView();
    this.hydrateDeviceFromCache();
    this.loadDeviceSummary({
      silent: true,
    });
  },

  onShow() {
    this.syncSettingsToView();
    this.hydrateDeviceFromCache();
  },

  syncSettingsToView() {
    const app = getApp();
    const settings = app.getSettings ? app.getSettings() : app.globalData.settings;
    const unitIndex = UNIT_OPTIONS.findIndex((item) => item.value === settings.temperatureUnit);
    const intervalIndex = ALLOWED_INTERVALS.indexOf(settings.autoRefreshIntervalSec);

    this.currentSettings = settings;
    this.setData({
      unitIndex: unitIndex >= 0 ? unitIndex : 0,
      intervalIndex: intervalIndex >= 0 ? intervalIndex : 0,
      currentUnitLabel: UNIT_OPTIONS[unitIndex >= 0 ? unitIndex : 0].label,
      currentIntervalLabel: `${settings.autoRefreshIntervalSec} 秒`,
      autoRefreshEnabled: !!settings.autoRefreshEnabled,
      thresholdInput: formatThresholdInput(settings.alarmThresholdC),
    });
  },

  hydrateDeviceFromCache() {
    const cached = getApp().globalData.lastDashboardData;

    if (!cached) {
      return;
    }

    const onlineStatus = normalizeOnlineStatus(cached.onlineStatus);

    this.setData({
      deviceDisplayName: cached.displayName || cached.deviceName || "未命名设备",
      nicknameInput: cached.nickname || cached.displayName || "",
      onlineStatus,
    });
  },

  loadDeviceSummary(options) {
    const app = getApp();

    if (!app.globalData.cloudReady || this.data.loadingDevice) {
      return;
    }

    this.setData({
      loadingDevice: true,
      errorMessage: options && options.silent ? this.data.errorMessage : "",
    });

    callIotBridge("getDashboardData")
      .then((data) => {
        app.globalData.lastDashboardData = data;
        this.hydrateDeviceFromCache();
      })
      .catch((error) => {
        this.setData({
          errorMessage: error.message || "设备摘要加载失败，请稍后重试。",
        });
      })
      .finally(() => {
        this.setData({
          loadingDevice: false,
        });
      });
  },

  persistSettings(patch, toastTitle) {
    const app = getApp();
    const nextSettings = app.updateSettings ? app.updateSettings(patch) : app.setSettings(patch);
    const unitIndex = UNIT_OPTIONS.findIndex((item) => item.value === nextSettings.temperatureUnit);
    const intervalIndex = ALLOWED_INTERVALS.indexOf(nextSettings.autoRefreshIntervalSec);

    this.currentSettings = nextSettings;
    this.setData({
      unitIndex: unitIndex >= 0 ? unitIndex : 0,
      intervalIndex: intervalIndex >= 0 ? intervalIndex : 0,
      currentUnitLabel: UNIT_OPTIONS[unitIndex >= 0 ? unitIndex : 0].label,
      currentIntervalLabel: `${nextSettings.autoRefreshIntervalSec} 秒`,
      autoRefreshEnabled: !!nextSettings.autoRefreshEnabled,
      thresholdInput: formatThresholdInput(nextSettings.alarmThresholdC),
    });

    if (toastTitle) {
      wx.showToast({
        title: toastTitle,
        icon: "none",
      });
    }
  },

  onUnitChange(event) {
    const next = UNIT_OPTIONS[Number(event.detail.value)] || UNIT_OPTIONS[0];

    this.persistSettings(
      {
        temperatureUnit: next.value,
      },
      `已切换为 ${next.label}`
    );
  },

  onAutoRefreshChange(event) {
    const enabled = !!event.detail.value;

    this.persistSettings(
      {
        autoRefreshEnabled: enabled,
      },
      enabled ? "已开启自动刷新" : "已关闭自动刷新"
    );
  },

  onIntervalChange(event) {
    const interval = ALLOWED_INTERVALS[Number(event.detail.value)] || DEFAULT_SETTINGS.autoRefreshIntervalSec;

    this.persistSettings(
      {
        autoRefreshIntervalSec: interval,
      },
      `刷新间隔已改为 ${interval} 秒`
    );
  },

  onThresholdInput(event) {
    this.setData({
      thresholdInput: event.detail.value,
    });
  },

  onSaveThresholdTap() {
    this.persistSettings(
      {
        alarmThresholdC: normalizeThreshold(this.data.thresholdInput),
      },
      "报警阈值已保存"
    );
  },

  onResetThresholdTap() {
    this.persistSettings(
      {
        alarmThresholdC: DEFAULT_SETTINGS.alarmThresholdC,
      },
      "已恢复默认阈值"
    );
  },

  onNicknameInput(event) {
    this.setData({
      nicknameInput: event.detail.value,
    });
  },

  onSaveNicknameTap() {
    const nickname = (this.data.nicknameInput || "").trim();

    if (this.data.savingNickname) {
      return;
    }

    this.setData({
      savingNickname: true,
      errorMessage: "",
    });

    callIotBridge("updateDeviceNickname", {
      nickname,
    })
      .then((data) => {
        const app = getApp();
        const cached = app.globalData.lastDashboardData || {};

        app.globalData.lastDashboardData = {
          ...cached,
          displayName: data.displayName,
          nickname: data.nickname,
          deviceName: data.deviceName || cached.deviceName,
        };

        this.hydrateDeviceFromCache();
        wx.showToast({
          title: "设备名称已更新",
          icon: "success",
        });
      })
      .catch((error) => {
        this.setData({
          errorMessage: error.message || "设备名称更新失败，请稍后重试。",
        });
      })
      .finally(() => {
        this.setData({
          savingNickname: false,
        });
      });
  },

  onBleProvisionTap() {
    wx.navigateTo({
      url: "/pages/ble-provision/index",
    });
  },

  onVersionTap() {
    const app = getApp();
    const now = Date.now();

    if (app.canAccessDebugPage && app.canAccessDebugPage()) {
      this.goDebugPage();
      return;
    }

    if (this.lastVersionTapAt && now - this.lastVersionTapAt < 1500) {
      this.versionTapCount = (this.versionTapCount || 0) + 1;
    } else {
      this.versionTapCount = 1;
    }

    this.lastVersionTapAt = now;

    if (this.versionTapCount >= 5) {
      app.grantDebugPageAccess();
      this.versionTapCount = 0;
      this.setData({
        debugHintText: "调试页已解锁，正在进入…",
      });
      this.goDebugPage();
      return;
    }

    this.setData({
      debugHintText: `再点击 ${5 - this.versionTapCount} 次进入隐藏调试页`,
    });
  },

  goDebugPage() {
    wx.navigateTo({
      url: "/pages/debug/index",
    });
  },
});
