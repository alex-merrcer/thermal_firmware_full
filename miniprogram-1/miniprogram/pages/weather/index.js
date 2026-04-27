const { callIotBridge } = require("../../utils/cloud");

function buildEmptyWeatherView() {
  return {
    cityName: "--",
    text: "--",
    temperatureText: "--",
    feelsLikeText: "--",
    humidityText: "--",
    windText: "--",
    updateText: "--",
    sourceText: "微信云函数",
  };
}

function formatNumberText(value, suffix) {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return "--";
  }

  return `${value}${suffix || ""}`;
}

function buildWeatherView(rawData) {
  const data = rawData || {};

  return {
    cityName: data.cityName || data.requestedLocation || "--",
    text: data.text || "--",
    temperatureText: formatNumberText(data.temperatureC, "°C"),
    feelsLikeText: formatNumberText(data.feelsLikeC, "°C"),
    humidityText: formatNumberText(data.humidity, "%"),
    windText: data.windScale ? `${data.windScale} 级` : "--",
    updateText: data.updateTime || "--",
    sourceText: data.sourceText || "微信云函数",
  };
}

Page({
  data: {
    loading: false,
    errorMessage: "",
    cityInput: "",
    weather: buildEmptyWeatherView(),
    usingDefaultCity: true,
  },

  onLoad() {
    const app = getApp();
    const settings = app.getSettings ? app.getSettings() : app.globalData.settings;
    const cityName = settings && settings.weatherCityName ? settings.weatherCityName : "";

    this.setData({
      cityInput: cityName,
      usingDefaultCity: cityName === "",
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
    const cityName = (this.data.cityInput || "").trim();

    if (app.updateSettings) {
      app.updateSettings({
        weatherCityName: cityName,
      });
    }

    this.setData({
      cityInput: cityName,
      usingDefaultCity: cityName === "",
    });

    wx.showToast({
      title: cityName ? "已保存城市" : "已改为默认城市",
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
      cityInput: "",
      usingDefaultCity: true,
    });

    wx.showToast({
      title: "已切换为默认城市",
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
    const cityName = (this.data.cityInput || "").trim();

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
      usingDefaultCity: cityName === "",
    });

    callIotBridge("getWeatherNow", cityName ? { cityName } : {})
      .then((data) => {
        this.setData({
          weather: buildWeatherView(data),
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
