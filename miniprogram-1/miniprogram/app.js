const { loadSettings, saveSettings } = require("./utils/settings");

const PLACEHOLDER_ENV_ID = "YOUR_CLOUD_ENV_ID";
const DEFAULT_ENV_ID = "cloud1-d1gfti0vgc196d61b";
const DEBUG_ACCESS_DURATION_MS = 5 * 60 * 1000;

App({
  onLaunch() {
    const settings = loadSettings();

    this.globalData = {
      envId: DEFAULT_ENV_ID,
      env: DEFAULT_ENV_ID,
      cloudFunctionName: "iotBridge",
      cloudReady: false,
      appVersion: "v2.0.0",
      deviceModelText: "STM32 + ESP32 红外热成像测温系统",
      settings,
      lastDashboardData: null,
      lastBridgeError: "",
      debugAccessUntilMs: 0,
    };

    if (!wx.cloud) {
      console.error("Please use a base library version that supports cloud capability.");
      return;
    }

    if (!this.globalData.envId || this.globalData.envId === PLACEHOLDER_ENV_ID) {
      console.warn("Fill miniprogram/app.js envId before running this mini program.");
      return;
    }

    wx.cloud.init({
      env: this.globalData.envId,
      traceUser: true,
    });

    this.globalData.cloudReady = true;
  },

  getSettings() {
    return this.globalData.settings;
  },

  setSettings(nextSettings) {
    const normalized = saveSettings(nextSettings);
    this.globalData.settings = normalized;
    return normalized;
  },

  updateSettings(patch) {
    return this.setSettings({
      ...this.getSettings(),
      ...patch,
    });
  },

  setLastBridgeError(message) {
    this.globalData.lastBridgeError = message || "";
  },

  clearLastBridgeError() {
    this.globalData.lastBridgeError = "";
  },

  grantDebugPageAccess() {
    this.globalData.debugAccessUntilMs = Date.now() + DEBUG_ACCESS_DURATION_MS;
  },

  canAccessDebugPage() {
    return Date.now() < (this.globalData.debugAccessUntilMs || 0);
  },
});
