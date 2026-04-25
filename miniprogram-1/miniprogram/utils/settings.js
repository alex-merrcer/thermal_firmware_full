const STORAGE_KEY = "thermal-mini-program-settings";
const DEFAULT_SETTINGS = {
  temperatureUnit: "C",
  autoRefreshEnabled: false,
  autoRefreshIntervalSec: 15,
  alarmThresholdC: 50,
};
const ALLOWED_INTERVALS = [5, 10, 15, 30, 60];

function normalizeThreshold(value) {
  const parsed = Number(value);

  if (!Number.isFinite(parsed)) {
    return DEFAULT_SETTINGS.alarmThresholdC;
  }

  return Math.min(200, Math.max(-40, Number(parsed.toFixed(1))));
}

function normalizeInterval(value) {
  const parsed = Number(value);

  if (!ALLOWED_INTERVALS.includes(parsed)) {
    return DEFAULT_SETTINGS.autoRefreshIntervalSec;
  }

  return parsed;
}

function normalizeSettings(rawSettings) {
  const raw = rawSettings || {};

  return {
    temperatureUnit: raw.temperatureUnit === "F" ? "F" : "C",
    autoRefreshEnabled: !!raw.autoRefreshEnabled,
    autoRefreshIntervalSec: normalizeInterval(raw.autoRefreshIntervalSec),
    alarmThresholdC: normalizeThreshold(raw.alarmThresholdC),
  };
}

function loadSettings() {
  try {
    const stored = wx.getStorageSync(STORAGE_KEY);
    return normalizeSettings(stored);
  } catch (error) {
    return { ...DEFAULT_SETTINGS };
  }
}

function saveSettings(nextSettings) {
  const normalized = normalizeSettings(nextSettings);

  try {
    wx.setStorageSync(STORAGE_KEY, normalized);
  } catch (error) {
    console.warn("保存本地设置失败", error);
  }

  return normalized;
}

module.exports = {
  STORAGE_KEY,
  DEFAULT_SETTINGS,
  ALLOWED_INTERVALS,
  loadSettings,
  normalizeSettings,
  normalizeThreshold,
  saveSettings,
};
