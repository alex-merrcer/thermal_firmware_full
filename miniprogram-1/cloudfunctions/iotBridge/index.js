const cloud = require("wx-server-sdk");
const crypto = require("crypto");
const https = require("https");
const { URL } = require("url");

const { ALIYUN_IOT_CONFIG, WEATHER_CONFIG } = require("./config");

cloud.init({
  env: cloud.DYNAMIC_CURRENT_ENV,
});

const PLACEHOLDER_VALUES = [
  "YOUR_ACCESS_KEY_ID",
  "YOUR_ACCESS_KEY_SECRET",
  "YOUR_PRODUCT_KEY",
  "YOUR_DEVICE_NAME",
];
const WEATHER_PLACEHOLDER_VALUES = [
  "YOUR_WEATHER_API_KEY",
  "your_weather_api_key",
];
const WEATHER_LIFE_INDEX_KEYS = ["comfort", "umbrella", "travel"];
const HISTORY_RANGE_MAP = {
  "1h": {
    durationMs: 60 * 60 * 1000,
    maxPages: 2,
    targetPoints: 24,
  },
  "24h": {
    durationMs: 24 * 60 * 60 * 1000,
    maxPages: 4,
    targetPoints: 36,
  },
  "7d": {
    durationMs: 7 * 24 * 60 * 60 * 1000,
    maxPages: 8,
    targetPoints: 42,
  },
};
const METRIC_IDENTIFIER_MAP = {
  minTemp: "minTemp",
  maxTemp: "maxTemp",
  centerTemp: "centerTemp",
};

function isPlaceholder(value) {
  return typeof value === "string" && PLACEHOLDER_VALUES.includes(value.trim());
}

function validateIotConfig() {
  const missing = [];

  if (!ALIYUN_IOT_CONFIG.accessKeyId || isPlaceholder(ALIYUN_IOT_CONFIG.accessKeyId)) {
    missing.push("accessKeyId");
  }
  if (!ALIYUN_IOT_CONFIG.accessKeySecret || isPlaceholder(ALIYUN_IOT_CONFIG.accessKeySecret)) {
    missing.push("accessKeySecret");
  }
  if (!ALIYUN_IOT_CONFIG.regionId) {
    missing.push("regionId");
  }
  if (!ALIYUN_IOT_CONFIG.productKey || isPlaceholder(ALIYUN_IOT_CONFIG.productKey)) {
    missing.push("productKey");
  }
  if (!ALIYUN_IOT_CONFIG.deviceName || isPlaceholder(ALIYUN_IOT_CONFIG.deviceName)) {
    missing.push("deviceName");
  }
  if (!ALIYUN_IOT_CONFIG.propertyIdentifiers || !ALIYUN_IOT_CONFIG.propertyIdentifiers.minTemp) {
    missing.push("propertyIdentifiers.minTemp");
  }
  if (!ALIYUN_IOT_CONFIG.propertyIdentifiers || !ALIYUN_IOT_CONFIG.propertyIdentifiers.maxTemp) {
    missing.push("propertyIdentifiers.maxTemp");
  }
  if (!ALIYUN_IOT_CONFIG.propertyIdentifiers || !ALIYUN_IOT_CONFIG.propertyIdentifiers.centerTemp) {
    missing.push("propertyIdentifiers.centerTemp");
  }

  return missing;
}

function isWeatherPlaceholder(value) {
  return typeof value === "string" && WEATHER_PLACEHOLDER_VALUES.includes(value.trim());
}

function validateWeatherConfig() {
  const missing = [];

  if (!WEATHER_CONFIG || !WEATHER_CONFIG.baseUrl) {
    missing.push("weather.baseUrl");
  }

  return missing;
}

function buildConfigMissingResult(missingFields) {
  return {
    success: false,
    errorCode: "CONFIG_MISSING",
    message: "Fill cloudfunctions/iotBridge/config.js before deploying this function.",
    missingFields,
  };
}

function percentEncode(value) {
  return encodeURIComponent(value)
    .replace(/\+/g, "%20")
    .replace(/\*/g, "%2A")
    .replace(/%7E/g, "~");
}

function buildTimestamp() {
  return new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
}

function buildSignatureNonce() {
  return crypto.randomBytes(16).toString("hex");
}

function buildRpcSignature(canonicalizedQueryString) {
  const stringToSign = `GET&${percentEncode("/")}&${percentEncode(canonicalizedQueryString)}`;

  return crypto
    .createHmac("sha1", `${ALIYUN_IOT_CONFIG.accessKeySecret}&`)
    .update(stringToSign)
    .digest("base64");
}

function buildRpcQuery(action, extraParams) {
  const baseParams = {
    Action: action,
    Format: "JSON",
    Version: ALIYUN_IOT_CONFIG.apiVersion || "2018-01-20",
    AccessKeyId: ALIYUN_IOT_CONFIG.accessKeyId,
    SignatureMethod: "HMAC-SHA1",
    SignatureVersion: "1.0",
    SignatureNonce: buildSignatureNonce(),
    Timestamp: buildTimestamp(),
  };
  const mergedParams = {
    ...baseParams,
    ...(extraParams || {}),
  };

  if (ALIYUN_IOT_CONFIG.iotInstanceId) {
    mergedParams.IotInstanceId = ALIYUN_IOT_CONFIG.iotInstanceId;
  }

  const sortedKeys = Object.keys(mergedParams).sort();
  const canonicalizedQueryString = sortedKeys
    .map((key) => `${percentEncode(key)}=${percentEncode(String(mergedParams[key]))}`)
    .join("&");
  const signature = buildRpcSignature(canonicalizedQueryString);

  return `${canonicalizedQueryString}&Signature=${percentEncode(signature)}`;
}

function normalizeRpcEndpoint(endpoint) {
  let normalized = typeof endpoint === "string" ? endpoint.trim() : "";

  if (!normalized) {
    normalized = `https://iot.${ALIYUN_IOT_CONFIG.regionId}.aliyuncs.com/`;
  } else {
    if (!/^https?:\/\//i.test(normalized)) {
      normalized = `https://${normalized}`;
    }
    if (!/\/$/.test(normalized)) {
      normalized = `${normalized}/`;
    }
  }

  return normalized;
}

function getRpcEndpoint() {
  return normalizeRpcEndpoint(ALIYUN_IOT_CONFIG.endpoint);
}

function requestJson(urlString, options) {
  const requestOptions = options || {};
  const headers = requestOptions.headers || {};

  return new Promise((resolve, reject) => {
    const url = new URL(urlString);
    const req = https.request(
      {
        protocol: url.protocol,
        hostname: url.hostname,
        path: `${url.pathname}${url.search}`,
        method: "GET",
        timeout: requestOptions.timeoutMs || ALIYUN_IOT_CONFIG.requestTimeoutMs || 8000,
        headers,
      },
      (res) => {
        let raw = "";

        res.setEncoding("utf8");
        res.on("data", (chunk) => {
          raw += chunk;
        });
        res.on("end", () => {
          if (res.statusCode < 200 || res.statusCode >= 300) {
            reject(new Error(`HTTP ${res.statusCode}: ${raw || "empty response body"}`));
            return;
          }

          try {
            resolve(JSON.parse(raw));
          } catch (error) {
            reject(new Error(`JSON parse failed: ${error.message}`));
          }
        });
      }
    );

    req.on("timeout", () => {
      req.destroy(new Error("Aliyun IoT request timeout"));
    });
    req.on("error", (error) => {
      reject(error);
    });
    req.end();
  });
}

function normalizeWeatherEndpoint(endpoint) {
  let normalized = typeof endpoint === "string" ? endpoint.trim() : "";

  if (!normalized) {
    normalized = "https://uapis.cn/api/v1/misc/weather";
  } else if (!/^https?:\/\//i.test(normalized)) {
    normalized = `https://${normalized}`;
  }

  return normalized.replace(/\/+$/, "");
}

function buildWeatherEndpointUrl() {
  const url = new URL(normalizeWeatherEndpoint(WEATHER_CONFIG.baseUrl));
  return url;
}

function getWeatherDefaultLocation() {
  return (WEATHER_CONFIG && typeof WEATHER_CONFIG.defaultLocation === "string" && WEATHER_CONFIG.defaultLocation.trim()) ?
    WEATHER_CONFIG.defaultLocation.trim() :
    "佛山";
}

function getWeatherDefaultAdcode() {
  return (WEATHER_CONFIG && typeof WEATHER_CONFIG.defaultAdcode === "string" && WEATHER_CONFIG.defaultAdcode.trim()) ?
    WEATHER_CONFIG.defaultAdcode.trim() :
    "";
}

function buildWeatherNowUrl(options) {
  const url = buildWeatherEndpointUrl();
  const requestOptions = options || {};
  const cityName = typeof requestOptions.cityName === "string" ? requestOptions.cityName.trim() : "";
  const adcode = typeof requestOptions.adcode === "string" ? requestOptions.adcode.trim() : "";

  if (cityName) {
    url.searchParams.set("city", cityName);
  } else if (adcode) {
    url.searchParams.set("adcode", adcode);
  } else {
    url.searchParams.set("city", getWeatherDefaultLocation());
  }

  if (adcode) {
    url.searchParams.set("adcode", adcode);
  }

  url.searchParams.set("lang", WEATHER_CONFIG.lang || "zh");
  url.searchParams.set("extended", WEATHER_CONFIG.enableExtended === false ? "false" : "true");
  url.searchParams.set("forecast", WEATHER_CONFIG.enableForecast === false ? "false" : "true");
  url.searchParams.set("hourly", WEATHER_CONFIG.enableHourly === false ? "false" : "true");
  url.searchParams.set("indices", WEATHER_CONFIG.enableIndices === false ? "false" : "true");

  if (WEATHER_CONFIG.apiKey && !isWeatherPlaceholder(WEATHER_CONFIG.apiKey)) {
    url.searchParams.set("apikey", WEATHER_CONFIG.apiKey);
  }

  return url.toString();
}

function pickLifeIndices(rawLifeIndices) {
  return WEATHER_LIFE_INDEX_KEYS.map((key) => {
    const source = rawLifeIndices && rawLifeIndices[key] ? rawLifeIndices[key] : {};

    switch (key) {
      case "comfort":
        return {
          key,
          title: "舒适度",
          level: source.level || "--",
          brief: source.brief || "--",
          advice: source.advice || "",
        };

      case "umbrella":
        return {
          key,
          title: "雨具建议",
          level: source.level || "--",
          brief: source.brief || "--",
          advice: source.advice || "",
        };

      case "travel":
        return {
          key,
          title: "出行建议",
          level: source.level || "--",
          brief: source.brief || "--",
          advice: source.advice || "",
        };

      default:
        return {
          key,
          title: key,
          level: source.level || "--",
          brief: source.brief || "--",
          advice: source.advice || "",
        };
    }
  }).filter((item) => item.level !== "--" || item.brief !== "--" || item.advice);
}

function parseWeatherNowResponse(response, requestedLocation) {
  if (!response || !response.city || !response.weather) {
    throw new Error("Weather API response is missing required fields");
  }

  return {
    requestedLocation,
    provinceName: response.province || "",
    cityName: response.city || requestedLocation || "",
    districtName: response.district || "",
    adcode: response.adcode || "",
    text: response.weather || "",
    iconCode: response.weather_icon || "",
    temperatureC: Number.isFinite(Number(response.temperature)) ? Number(response.temperature) : null,
    tempHighC: Number.isFinite(Number(response.temp_max)) ? Number(response.temp_max) : null,
    tempLowC: Number.isFinite(Number(response.temp_min)) ? Number(response.temp_min) : null,
    feelsLikeC: Number.isFinite(Number(response.feels_like)) ? Number(response.feels_like) : null,
    humidity: Number.isFinite(Number(response.humidity)) ? Number(response.humidity) : null,
    windDirection: response.wind_direction || "",
    windPower: response.wind_power || "",
    visibilityKm: Number.isFinite(Number(response.visibility)) ? Number(response.visibility) : null,
    pressureHpa: Number.isFinite(Number(response.pressure)) ? Number(response.pressure) : null,
    uvIndex: Number.isFinite(Number(response.uv)) ? Number(response.uv) : null,
    aqi: Number.isFinite(Number(response.aqi)) ? Number(response.aqi) : null,
    aqiLevel: Number.isFinite(Number(response.aqi_level)) ? Number(response.aqi_level) : null,
    aqiCategory: response.aqi_category || "",
    aqiPrimary: response.aqi_primary || "",
    precipitationMm: Number.isFinite(Number(response.precipitation)) ? Number(response.precipitation) : null,
    cloudPercent: Number.isFinite(Number(response.cloud)) ? Number(response.cloud) : null,
    reportTimeText: response.report_time || "",
    forecast: Array.isArray(response.forecast)
      ? response.forecast.slice(0, 3).map((item) => ({
          date: item.date || "",
          week: item.week || "",
          tempMaxC: Number.isFinite(Number(item.temp_max)) ? Number(item.temp_max) : null,
          tempMinC: Number.isFinite(Number(item.temp_min)) ? Number(item.temp_min) : null,
          weatherDay: item.weather_day || "",
          weatherNight: item.weather_night || "",
          pop: Number.isFinite(Number(item.pop)) ? Number(item.pop) : null,
          sunrise: item.sunrise || "",
          sunset: item.sunset || "",
        }))
      : [],
    lifeIndices: pickLifeIndices(response.life_indices),
    hourlyForecast: Array.isArray(response.hourly_forecast)
      ? response.hourly_forecast.slice(0, 6).map((item) => ({
          time: item.time || "",
          temperatureC: Number.isFinite(Number(item.temperature)) ? Number(item.temperature) : null,
          weather: item.weather || "",
          humidity: Number.isFinite(Number(item.humidity)) ? Number(item.humidity) : null,
          pop: Number.isFinite(Number(item.pop)) ? Number(item.pop) : null,
        }))
      : [],
    sourceText: "UAPI 天气",
    fetchedAtMs: Date.now(),
  };
}

async function callAliyunIotRpc(action, params) {
  const query = buildRpcQuery(action, params);
  const requestUrl = `${getRpcEndpoint()}?${query}`;
  const response = await requestJson(requestUrl);

  if (response && response.Success === false) {
    throw new Error(response.ErrorMessage || response.Code || "Aliyun IoT API failed");
  }

  if (response && response.Code && response.Code !== "200") {
    throw new Error(response.ErrorMessage || response.Code);
  }

  return response;
}

function buildThingTargetParams() {
  return {
    ProductKey: ALIYUN_IOT_CONFIG.productKey,
    DeviceName: ALIYUN_IOT_CONFIG.deviceName,
  };
}

function buildPropertyPostTopic() {
  return `/sys/${ALIYUN_IOT_CONFIG.productKey}/${ALIYUN_IOT_CONFIG.deviceName}/thing/event/property/post`;
}

function parsePropertyValue(value) {
  if (value === null || value === undefined || value === "") {
    return null;
  }

  const numeric = Number(value);
  if (!Number.isNaN(numeric)) {
    return numeric;
  }

  return value;
}

function toNumberOrNull(value) {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function normalizePropertyStatusList(apiResponse) {
  const rawList =
    apiResponse &&
    apiResponse.Data &&
    apiResponse.Data.List &&
    apiResponse.Data.List.PropertyStatusInfo;

  if (Array.isArray(rawList)) {
    return rawList;
  }

  if (rawList && typeof rawList === "object") {
    return [rawList];
  }

  return [];
}

function normalizePropertyDataList(apiResponse) {
  const rawList =
    apiResponse &&
    apiResponse.Data &&
    apiResponse.Data.List &&
    apiResponse.Data.List.PropertyInfo;

  if (Array.isArray(rawList)) {
    return rawList;
  }

  if (rawList && typeof rawList === "object") {
    return [rawList];
  }

  return [];
}

function mapPropertyStatusByIdentifier(propertyList) {
  const propertyMap = {};

  propertyList.forEach((item) => {
    if (!item || !item.Identifier) {
      return;
    }

    propertyMap[item.Identifier] = {
      identifier: item.Identifier,
      name: item.Name || "",
      value: parsePropertyValue(item.Value),
      timeMs: item.Time ? Number(item.Time) : 0,
      unit: item.Unit || "",
    };
  });

  return propertyMap;
}

async function fetchThermalSnapshotCore() {
  const startedAt = Date.now();
  const response = await callAliyunIotRpc("QueryDevicePropertyStatus", buildThingTargetParams());
  const propertyList = normalizePropertyStatusList(response);
  const propertyMap = mapPropertyStatusByIdentifier(propertyList);
  const identifiers = ALIYUN_IOT_CONFIG.propertyIdentifiers;
  const minTemp = propertyMap[identifiers.minTemp];
  const maxTemp = propertyMap[identifiers.maxTemp];
  const centerTemp = propertyMap[identifiers.centerTemp];
  const latestPropertyTimeMs = Math.max(
    (minTemp && minTemp.timeMs) || 0,
    (maxTemp && maxTemp.timeMs) || 0,
    (centerTemp && centerTemp.timeMs) || 0
  );

  return {
    requestId: response.RequestId || "",
    response,
    durationMs: Date.now() - startedAt,
    identifiers,
    minTemp: toNumberOrNull(minTemp && minTemp.value),
    maxTemp: toNumberOrNull(maxTemp && maxTemp.value),
    centerTemp: toNumberOrNull(centerTemp && centerTemp.value),
    latestPropertyTimeMs,
    propertyList: propertyList.map((item) => ({
      identifier: item.Identifier || "",
      name: item.Name || "",
      value: parsePropertyValue(item.Value),
      unit: item.Unit || "",
      timeMs: item.Time ? Number(item.Time) : 0,
    })),
  };
}

async function fetchDeviceInfo() {
  const startedAt = Date.now();
  const response = await callAliyunIotRpc("QueryDeviceInfo", buildThingTargetParams());
  const data = response && response.Data ? response.Data : {};

  return {
    requestId: response.RequestId || "",
    response,
    durationMs: Date.now() - startedAt,
    nickname: data.Nickname || "",
    iotId: data.IotId || "",
    deviceName: data.DeviceName || ALIYUN_IOT_CONFIG.deviceName,
    productKey: data.ProductKey || ALIYUN_IOT_CONFIG.productKey,
  };
}

async function fetchDeviceStatus() {
  const startedAt = Date.now();
  const response = await callAliyunIotRpc("GetDeviceStatus", buildThingTargetParams());
  const data = response && response.Data ? response.Data : {};

  return {
    requestId: response.RequestId || "",
    response,
    durationMs: Date.now() - startedAt,
    status: data.Status || data.DeviceStatus || "UNKNOWN",
  };
}

function normalizeMetricKey(metric) {
  return Object.prototype.hasOwnProperty.call(METRIC_IDENTIFIER_MAP, metric) ? metric : "maxTemp";
}

function normalizeRangeKey(range) {
  return Object.prototype.hasOwnProperty.call(HISTORY_RANGE_MAP, range) ? range : "1h";
}

function dedupeAndSortPoints(points) {
  const seen = new Map();

  (points || []).forEach((item) => {
    if (!item || !Number.isFinite(item.timeMs) || !Number.isFinite(item.value)) {
      return;
    }
    seen.set(item.timeMs, {
      timeMs: item.timeMs,
      value: item.value,
    });
  });

  return Array.from(seen.values()).sort((left, right) => left.timeMs - right.timeMs);
}

function samplePoints(points, targetCount) {
  if (!Array.isArray(points) || points.length <= targetCount) {
    return points || [];
  }

  const lastIndex = points.length - 1;
  const sampled = [];
  const picked = new Set();

  for (let index = 0; index < targetCount; index += 1) {
    const ratio = targetCount === 1 ? 0 : index / (targetCount - 1);
    const pointIndex = Math.round(ratio * lastIndex);

    if (picked.has(pointIndex)) {
      continue;
    }

    picked.add(pointIndex);
    sampled.push(points[pointIndex]);
  }

  if (!picked.has(0)) {
    sampled.unshift(points[0]);
  }
  if (!picked.has(lastIndex)) {
    sampled.push(points[lastIndex]);
  }

  return dedupeAndSortPoints(sampled);
}

async function queryThermalHistory(metric, range) {
  const metricKey = normalizeMetricKey(metric);
  const rangeKey = normalizeRangeKey(range);
  const rangeConfig = HISTORY_RANGE_MAP[rangeKey];
  const identifier = ALIYUN_IOT_CONFIG.propertyIdentifiers[METRIC_IDENTIFIER_MAP[metricKey]];
  const nowMs = Date.now();
  const startTime = nowMs - rangeConfig.durationMs;
  const requestIds = [];
  const points = [];
  let nextEndTime = nowMs;
  let pageIndex = 0;

  while (pageIndex < rangeConfig.maxPages) {
    const response = await callAliyunIotRpc("QueryDevicePropertyData", {
      ...buildThingTargetParams(),
      Identifier: identifier,
      StartTime: startTime,
      EndTime: nextEndTime,
      PageSize: 50,
      Asc: 0,
    });
    const pagePoints = normalizePropertyDataList(response);
    const nextValid =
      !!(response && response.Data && response.Data.NextValid);
    const nextTime =
      response && response.Data && response.Data.NextTime
        ? Number(response.Data.NextTime)
        : 0;

    requestIds.push(response.RequestId || "");
    pagePoints.forEach((item) => {
      const value = parsePropertyValue(item.Value);
      const timeMs = Number(item.Time);
      const numericValue = toNumberOrNull(value);

      if (!Number.isFinite(timeMs) || !Number.isFinite(numericValue)) {
        return;
      }

      if (timeMs >= startTime && timeMs <= nowMs) {
        points.push({
          timeMs,
          value: numericValue,
        });
      }
    });

    if (!nextValid || !Number.isFinite(nextTime) || nextTime <= startTime || nextTime >= nextEndTime) {
      break;
    }

    nextEndTime = nextTime;
    pageIndex += 1;
  }

  return {
    metric: metricKey,
    range: rangeKey,
    identifier,
    requestIds,
    points: samplePoints(dedupeAndSortPoints(points), rangeConfig.targetPoints),
    fetchedAtMs: Date.now(),
  };
}

function charDisplayLength(text) {
  return Array.from(text || "").reduce((sum, char) => {
    return sum + (char.charCodeAt(0) > 0xff ? 2 : 1);
  }, 0);
}

function isValidNickname(text) {
  const length = charDisplayLength(text);

  if (length < 4 || length > 32) {
    return false;
  }

  return /^[\u4e00-\u9fa5A-Za-z0-9_]+$/.test(text);
}

async function buildDashboardData(debugEnabled) {
  const startedAt = Date.now();
  const [snapshot, deviceInfo, deviceStatus] = await Promise.all([
    fetchThermalSnapshotCore(),
    fetchDeviceInfo(),
    fetchDeviceStatus(),
  ]);
  const displayName = deviceInfo.nickname || deviceInfo.deviceName || ALIYUN_IOT_CONFIG.deviceName;
  const data = {
    displayName,
    nickname: deviceInfo.nickname || "",
    iotId: deviceInfo.iotId || "",
    productKey: ALIYUN_IOT_CONFIG.productKey,
    deviceName: ALIYUN_IOT_CONFIG.deviceName,
    endpoint: getRpcEndpoint(),
    propertyPostTopic: buildPropertyPostTopic(),
    onlineStatus: deviceStatus.status,
    isOnline: String(deviceStatus.status || "").toUpperCase() === "ONLINE",
    minTemp: snapshot.minTemp,
    maxTemp: snapshot.maxTemp,
    centerTemp: snapshot.centerTemp,
    latestPropertyTimeMs: snapshot.latestPropertyTimeMs,
    fetchedAtMs: Date.now(),
  };

  if (debugEnabled) {
    data.debug = {
      requestIds: {
        propertyStatus: snapshot.requestId,
        deviceInfo: deviceInfo.requestId,
        deviceStatus: deviceStatus.requestId,
      },
      apiDurationsMs: {
        propertyStatus: snapshot.durationMs,
        deviceInfo: deviceInfo.durationMs,
        deviceStatus: deviceStatus.durationMs,
        total: Date.now() - startedAt,
      },
      propertyIdentifiers: snapshot.identifiers,
      propertyList: snapshot.propertyList,
      rawResponses: {
        propertyStatus: snapshot.response,
        deviceInfo: deviceInfo.response,
        deviceStatus: deviceStatus.response,
      },
    };
  }

  return data;
}

async function getThermalSnapshot() {
  const snapshot = await fetchThermalSnapshotCore();

  return {
    success: true,
    data: {
      productKey: ALIYUN_IOT_CONFIG.productKey,
      deviceName: ALIYUN_IOT_CONFIG.deviceName,
      regionId: ALIYUN_IOT_CONFIG.regionId,
      endpoint: getRpcEndpoint(),
      requestId: snapshot.requestId,
      identifiers: snapshot.identifiers,
      minTemp: snapshot.minTemp,
      maxTemp: snapshot.maxTemp,
      centerTemp: snapshot.centerTemp,
      latestPropertyTimeMs: snapshot.latestPropertyTimeMs,
      fetchedAtMs: Date.now(),
    },
  };
}

async function getDashboardData(event) {
  return {
    success: true,
    data: await buildDashboardData(!!(event && event.debug)),
  };
}

async function getThermalHistory(event) {
  return {
    success: true,
    data: await queryThermalHistory(event && event.metric, event && event.range),
  };
}

async function updateDeviceNickname(event) {
  const nickname = typeof (event && event.nickname) === "string" ? event.nickname.trim() : "";

  if (!nickname || !isValidNickname(nickname)) {
    return {
      success: false,
      errorCode: "INVALID_NICKNAME",
      message: "设备名称需为 4-32 个字符，可包含中文、字母、数字和下划线。",
    };
  }

  const response = await callAliyunIotRpc("BatchUpdateDeviceNickname", {
    "DeviceNicknameInfo.1.ProductKey": ALIYUN_IOT_CONFIG.productKey,
    "DeviceNicknameInfo.1.DeviceName": ALIYUN_IOT_CONFIG.deviceName,
    "DeviceNicknameInfo.1.Nickname": nickname,
  });
  const deviceInfo = await fetchDeviceInfo();

  return {
    success: true,
    data: {
      displayName: deviceInfo.nickname || ALIYUN_IOT_CONFIG.deviceName,
      nickname: deviceInfo.nickname || nickname,
      deviceName: ALIYUN_IOT_CONFIG.deviceName,
      requestId: response.RequestId || "",
      fetchedAtMs: Date.now(),
    },
  };
}

async function getDebugData() {
  return {
    success: true,
    data: await buildDashboardData(true),
  };
}

async function getWeatherNow(event) {
  const requestedLocation =
    typeof (event && event.cityName) === "string" && event.cityName.trim()
      ? event.cityName.trim()
      : getWeatherDefaultLocation();
  const requestedAdcode =
    typeof (event && event.adcode) === "string" && event.adcode.trim()
      ? event.adcode.trim()
      : getWeatherDefaultAdcode();
  const response = await requestJson(
    buildWeatherNowUrl({
      cityName: requestedLocation,
      adcode: requestedAdcode,
    }),
    {
      timeoutMs: WEATHER_CONFIG.requestTimeoutMs || 8000,
      headers: {
        "User-Agent": "Mozilla/5.0",
      },
    }
  );

  return {
    success: true,
    data: parseWeatherNowResponse(response, requestedLocation),
  };
}

async function ping() {
  return {
    success: true,
    data: {
      message: "iotBridge cloud function is alive",
      fetchedAtMs: Date.now(),
    },
  };
}

exports.main = async (event) => {
  const action = (event && event.action) || "getDashboardData";

  try {
    switch (action) {
      case "ping":
        return await ping();

      case "getThermalSnapshot":
        if (validateIotConfig().length > 0) {
          return buildConfigMissingResult(validateIotConfig());
        }
        return await getThermalSnapshot();

      case "getDashboardData":
        if (validateIotConfig().length > 0) {
          return buildConfigMissingResult(validateIotConfig());
        }
        return await getDashboardData(event);

      case "getThermalHistory":
        if (validateIotConfig().length > 0) {
          return buildConfigMissingResult(validateIotConfig());
        }
        return await getThermalHistory(event);

      case "updateDeviceNickname":
        if (validateIotConfig().length > 0) {
          return buildConfigMissingResult(validateIotConfig());
        }
        return await updateDeviceNickname(event);

      case "getDebugData":
        if (validateIotConfig().length > 0) {
          return buildConfigMissingResult(validateIotConfig());
        }
        return await getDebugData();

      case "getWeatherNow":
        if (validateWeatherConfig().length > 0) {
          return buildConfigMissingResult(validateWeatherConfig());
        }
        return await getWeatherNow(event);

      default:
        return {
          success: false,
          errorCode: "UNSUPPORTED_ACTION",
          message: `Unsupported action: ${action}`,
        };
    }
  } catch (error) {
    return {
      success: false,
      errorCode: "ALIYUN_IOT_REQUEST_FAILED",
      message: error.message,
    };
  }
};
