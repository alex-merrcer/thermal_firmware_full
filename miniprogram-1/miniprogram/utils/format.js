function isUsableNumber(value) {
  return typeof value === "number" && Number.isFinite(value);
}

function toFahrenheit(celsius) {
  if (!isUsableNumber(celsius)) {
    return null;
  }

  return (celsius * 9) / 5 + 32;
}

function convertTemperature(value, unit) {
  if (!isUsableNumber(value)) {
    return null;
  }

  return unit === "F" ? toFahrenheit(value) : value;
}

function formatTemperature(value, unit) {
  const normalized = convertTemperature(value, unit);

  if (!isUsableNumber(normalized)) {
    return "--.-";
  }

  return normalized.toFixed(1);
}

function getTemperatureUnitLabel(unit) {
  return unit === "F" ? "°F" : "°C";
}

function pad2(value) {
  return value < 10 ? `0${value}` : `${value}`;
}

function formatDateTime(timeMs) {
  if (!isUsableNumber(timeMs) || timeMs <= 0) {
    return "--";
  }

  const date = new Date(timeMs);

  if (Number.isNaN(date.getTime())) {
    return "--";
  }

  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())} ${pad2(
    date.getHours()
  )}:${pad2(date.getMinutes())}:${pad2(date.getSeconds())}`;
}

function formatShortTime(timeMs) {
  if (!isUsableNumber(timeMs) || timeMs <= 0) {
    return "--:--";
  }

  const date = new Date(timeMs);

  if (Number.isNaN(date.getTime())) {
    return "--:--";
  }

  return `${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
}

function formatRelativeUpdate(timeMs) {
  if (!isUsableNumber(timeMs) || timeMs <= 0) {
    return "暂无上报";
  }

  const diffMs = Date.now() - timeMs;

  if (diffMs < 60 * 1000) {
    return "刚刚更新";
  }

  if (diffMs < 60 * 60 * 1000) {
    return `${Math.max(1, Math.floor(diffMs / (60 * 1000)))} 分钟前更新`;
  }

  if (diffMs < 24 * 60 * 60 * 1000) {
    return `${Math.max(1, Math.floor(diffMs / (60 * 60 * 1000)))} 小时前更新`;
  }

  return `${Math.max(1, Math.floor(diffMs / (24 * 60 * 60 * 1000)))} 天前更新`;
}

function normalizeOnlineStatus(status) {
  if (typeof status !== "string") {
    return {
      text: "状态未知",
      tone: "pending",
      isOnline: false,
    };
  }

  switch (status.toUpperCase()) {
    case "ONLINE":
      return {
        text: "在线",
        tone: "online",
        isOnline: true,
      };

    case "OFFLINE":
      return {
        text: "离线",
        tone: "offline",
        isOnline: false,
      };

    case "UNACTIVE":
      return {
        text: "未激活",
        tone: "pending",
        isOnline: false,
      };

    case "DISABLE":
      return {
        text: "已禁用",
        tone: "offline",
        isOnline: false,
      };

    default:
      return {
        text: "状态未知",
        tone: "pending",
        isOnline: false,
      };
  }
}

function buildThermalStatus(maxTempC, isOnline, alarmThresholdC) {
  const warningThresholdC = Number((alarmThresholdC - 5).toFixed(1));

  if (!isOnline) {
    return {
      code: "offline",
      label: "设备离线",
      description: "设备当前未在线，请检查网络连接、电源或阿里云设备状态。",
      tone: "offline",
    };
  }

  if (!isUsableNumber(maxTempC)) {
    return {
      code: "pending",
      label: "等待数据",
      description: "已连接云端，正在等待新的热成像温度快照。",
      tone: "pending",
    };
  }

  if (maxTempC >= alarmThresholdC) {
    return {
      code: "alert",
      label: "报警",
      description: `最高温已达到 ${maxTempC.toFixed(1)}°C，请及时检查现场设备或被测区域。`,
      tone: "alert",
    };
  }

  if (maxTempC >= warningThresholdC) {
    return {
      code: "warning",
      label: "温度偏高",
      description: `最高温接近报警阈值，建议重点关注高温区域变化。`,
      tone: "warning",
    };
  }

  return {
    code: "normal",
    label: "正常",
    description: "当前温度处于设定阈值范围内，设备状态稳定。",
    tone: "normal",
  };
}

function formatMetricLabel(metric) {
  switch (metric) {
    case "maxTemp":
      return "最高温";
    case "centerTemp":
      return "中心温";
    case "minTemp":
      return "最低温";
    default:
      return "温度";
  }
}

function formatRangeLabel(range) {
  switch (range) {
    case "1h":
      return "最近 1 小时";
    case "24h":
      return "最近 24 小时";
    case "7d":
      return "最近 7 天";
    default:
      return "历史趋势";
  }
}

function safeJSONStringify(value) {
  try {
    return JSON.stringify(value, null, 2);
  } catch (error) {
    return "无法序列化调试数据";
  }
}

module.exports = {
  buildThermalStatus,
  convertTemperature,
  formatDateTime,
  formatMetricLabel,
  formatRangeLabel,
  formatRelativeUpdate,
  formatShortTime,
  formatTemperature,
  getTemperatureUnitLabel,
  isUsableNumber,
  normalizeOnlineStatus,
  safeJSONStringify,
};
