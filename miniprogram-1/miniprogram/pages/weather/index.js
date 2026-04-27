const { callIotBridge } = require("../../utils/cloud");

const DEFAULT_CITY_NAME = "佛山";

function formatNumberText(value, suffix) {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return "--";
  }

  return `${value}${suffix || ""}`;
}

function buildEmptyWeatherView() {
  return {
    cityName: DEFAULT_CITY_NAME,
    locationText: "默认城市",
    text: "--",
    temperatureText: "--",
    rangeText: "-- / --",
    reportText: "--",
    airText: "--",
    summaryItems: [],
    forecastItems: [],
    lifeItems: [],
    hourlyItems: [],
  };
}

function buildSummaryItems(data) {
  return [
    {
      key: "feelsLike",
      label: "体感温度",
      value: formatNumberText(data.feelsLikeC, "°C"),
    },
    {
      key: "humidity",
      label: "湿度",
      value: formatNumberText(data.humidity, "%"),
    },
    {
      key: "wind",
      label: "风向风力",
      value:
        data.windDirection || data.windPower
          ? `${data.windDirection || "--"} ${data.windPower || ""}`.trim()
          : "--",
    },
    {
      key: "visibility",
      label: "能见度",
      value: formatNumberText(data.visibilityKm, " km"),
    },
    {
      key: "pressure",
      label: "气压",
      value: formatNumberText(data.pressureHpa, " hPa"),
    },
    {
      key: "precipitation",
      label: "降水量",
      value: formatNumberText(data.precipitationMm, " mm"),
    },
  ];
}

function buildForecastItems(data) {
  return Array.isArray(data.forecast)
    ? data.forecast.map((item) => ({
        key: `${item.date}-${item.week}`,
        week: item.week || "--",
        date: item.date || "--",
        weatherText: item.weatherDay
          ? item.weatherNight && item.weatherNight !== item.weatherDay
            ? `${item.weatherDay} / ${item.weatherNight}`
            : item.weatherDay
          : "--",
        tempText: `${formatNumberText(item.tempMinC, "°")} ~ ${formatNumberText(item.tempMaxC, "°")}`,
        extraText:
          typeof item.pop === "number" && Number.isFinite(item.pop)
            ? `降水概率 ${item.pop}%`
            : item.sunrise || item.sunset
            ? `${item.sunrise || "--"} / ${item.sunset || "--"}`
            : "--",
      }))
    : [];
}

function buildLifeItems(data) {
  return Array.isArray(data.lifeIndices)
    ? data.lifeIndices.map((item) => ({
        key: item.key,
        title: item.title || "--",
        brief: item.brief || item.level || "--",
        advice: item.advice || "",
      }))
    : [];
}

function buildHourlyItems(data) {
  return Array.isArray(data.hourlyForecast)
    ? data.hourlyForecast.map((item) => ({
        key: `${item.time}-${item.weather}`,
        timeText: item.time ? item.time.slice(11, 16) : "--:--",
        weatherText: item.weather || "--",
        tempText: formatNumberText(item.temperatureC, "°C"),
        rainText:
          typeof item.pop === "number" && Number.isFinite(item.pop) ? `降水 ${item.pop}%` : "--",
      }))
    : [];
}

function buildWeatherView(rawData, options) {
  const data = rawData || {};
  const viewOptions = options || {};
  const locationParts = [data.provinceName, data.cityName, data.districtName].filter(Boolean);
  const locationText = locationParts.length ? locationParts.join(" ") : viewOptions.locationLabel || "默认城市";
  const airText =
    typeof data.aqi === "number" && Number.isFinite(data.aqi)
      ? `AQI ${data.aqi}${data.aqiCategory ? ` · ${data.aqiCategory}` : ""}`
      : "--";

  return {
    cityName: data.cityName || viewOptions.fallbackCityName || DEFAULT_CITY_NAME,
    locationText,
    text: data.text || "--",
    temperatureText: formatNumberText(data.temperatureC, "°C"),
    rangeText: `${formatNumberText(data.tempLowC, "°C")} / ${formatNumberText(data.tempHighC, "°C")}`,
    reportText: data.reportTimeText || "--",
    airText,
    summaryItems: buildSummaryItems(data),
    forecastItems: buildForecastItems(data),
    lifeItems: buildLifeItems(data),
    hourlyItems: buildHourlyItems(data),
  };
}

Page({
  data: {
    loading: false,
    errorMessage: "",
    cityInput: DEFAULT_CITY_NAME,
    weather: buildEmptyWeatherView(),
    usingDefaultCity: true,
  },

  onLoad() {
    const app = getApp();
    const settings = app.getSettings ? app.getSettings() : app.globalData.settings;
    const savedCityName = settings && settings.weatherCityName ? settings.weatherCityName : "";
    const initialCityName = savedCityName || DEFAULT_CITY_NAME;

    this.setData({
      cityInput: initialCityName,
      usingDefaultCity: savedCityName === "",
      weather: buildWeatherView({}, {
        fallbackCityName: initialCityName,
        locationLabel: savedCityName ? "已保存城市" : "默认城市",
      }),
    });

    this.loadWeather({
      silent: true,
    });
  },

  onPullDownRefresh() {
    this.loadWeather({
      fromPullDown: true,
    });
  },

  onCityInput(event) {
    this.setData({
      cityInput: event.detail.value,
    });
  },

  onSaveCityTap() {
    const app = getApp();
    const cityName = (this.data.cityInput || "").trim() || DEFAULT_CITY_NAME;

    if (app.updateSettings) {
      app.updateSettings({
        weatherCityName: cityName === DEFAULT_CITY_NAME ? "" : cityName,
      });
    }

    this.setData({
      cityInput: cityName,
      usingDefaultCity: cityName === DEFAULT_CITY_NAME,
    });

    wx.showToast({
      title: cityName === DEFAULT_CITY_NAME ? "已恢复默认城市" : "已保存城市",
      icon: "none",
    });

    this.loadWeather({
      silent: true,
    });
  },

  onUseDefaultCityTap() {
    const app = getApp();

    if (app.updateSettings) {
      app.updateSettings({
        weatherCityName: "",
      });
    }

    this.setData({
      cityInput: DEFAULT_CITY_NAME,
      usingDefaultCity: true,
    });

    wx.showToast({
      title: "已切换为佛山",
      icon: "none",
    });

    this.loadWeather({
      silent: true,
    });
  },

  onRefreshTap() {
    this.loadWeather();
  },

  loadWeather(options) {
    const app = getApp();
    const cityName = (this.data.cityInput || "").trim() || DEFAULT_CITY_NAME;

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
      usingDefaultCity: cityName === DEFAULT_CITY_NAME,
    });

    callIotBridge("getWeatherNow", {
      cityName,
    })
      .then((data) => {
        this.setData({
          weather: buildWeatherView(data, {
            fallbackCityName: cityName,
            locationLabel: cityName === DEFAULT_CITY_NAME ? "默认城市" : "已保存城市",
          }),
          errorMessage: "",
        });
      })
      .catch((error) => {
        this.setData({
          errorMessage: error.message || "天气获取失败，请稍后重试。",
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
