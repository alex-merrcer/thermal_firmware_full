const { callIotBridge } = require("../../utils/cloud");
const {
  convertTemperature,
  formatDateTime,
  formatMetricLabel,
  formatRangeLabel,
  getTemperatureUnitLabel,
  isUsableNumber,
} = require("../../utils/format");

const METRIC_OPTIONS = [
  { label: "最高温", value: "maxTemp" },
  { label: "中心温", value: "centerTemp" },
  { label: "最低温", value: "minTemp" },
];
const RANGE_OPTIONS = [
  { label: "1 小时", value: "1h" },
  { label: "24 小时", value: "24h" },
  { label: "7 天", value: "7d" },
];
const METRIC_COLOR_MAP = {
  maxTemp: "#d86a38",
  centerTemp: "#d59c32",
  minTemp: "#5785f7",
};

function buildOptionState(options, selectedValue) {
  return options.map((item) => ({
    ...item,
    active: item.value === selectedValue,
  }));
}

function pad2(value) {
  return value < 10 ? `0${value}` : `${value}`;
}

function formatAxisTime(timeMs, range) {
  const date = new Date(timeMs);

  if (Number.isNaN(date.getTime())) {
    return "--";
  }

  if (range === "7d") {
    return `${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
  }

  if (range === "24h") {
    return `${pad2(date.getDate())}日 ${pad2(date.getHours())}:00`;
  }

  return `${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
}

function uniqueIndexes(indexes, maxLength) {
  const picked = [];
  const seen = {};

  indexes.forEach((value) => {
    if (value < 0 || value >= maxLength || seen[value]) {
      return;
    }

    seen[value] = true;
    picked.push(value);
  });

  return picked;
}

function buildHistoryViewModel(rawData, settings) {
  const normalizedRaw = rawData || {};
  const normalizedSettings = settings || {
    temperatureUnit: "C",
  };
  const unit = normalizedSettings.temperatureUnit;
  const points = (Array.isArray(normalizedRaw.points) ? normalizedRaw.points : [])
    .map((item) => ({
      timeMs: Number(item.timeMs),
      value: convertTemperature(item.value, unit),
    }))
    .filter((item) => Number.isFinite(item.timeMs) && isUsableNumber(item.value));
  const values = points.map((item) => item.value);
  const latestPoint = points.length ? points[points.length - 1] : null;

  return {
    metric: normalizedRaw.metric || "maxTemp",
    range: normalizedRaw.range || "1h",
    rangeLabel: formatRangeLabel(normalizedRaw.range),
    metricLabel: formatMetricLabel(normalizedRaw.metric),
    unitLabel: getTemperatureUnitLabel(unit),
    points,
    latestValueText: latestPoint ? latestPoint.value.toFixed(1) : "--.-",
    minValueText: values.length ? Math.min.apply(null, values).toFixed(1) : "--.-",
    maxValueText: values.length ? Math.max.apply(null, values).toFixed(1) : "--.-",
    sampleCountText: `${points.length}`,
    updatedAtText: formatDateTime(normalizedRaw.fetchedAtMs),
    emptyText: "当前时间范围内暂无历史温度数据。",
  };
}

Page({
  data: {
    loading: false,
    errorMessage: "",
    metric: "maxTemp",
    range: "1h",
    metricOptions: buildOptionState(METRIC_OPTIONS, "maxTemp"),
    rangeOptions: buildOptionState(RANGE_OPTIONS, "1h"),
    chartWidth: 320,
    chartHeight: 250,
    hasData: false,
    metricLabel: "最高温",
    rangeLabel: "最近 1 小时",
    latestValueText: "--.-",
    minValueText: "--.-",
    maxValueText: "--.-",
    sampleCountText: "0",
    updatedAtText: "--",
    unitLabel: "°C",
    emptyText: "当前时间范围内暂无历史温度数据。",
  },

  onLoad() {
    this.initChartSize();
    this.syncSettings();
  },

  onShow() {
    if (this.rawHistoryData) {
      this.applyHistoryData(this.rawHistoryData);
      return;
    }

    this.loadHistory();
  },

  onPullDownRefresh() {
    this.loadHistory({
      fromPullDown: true,
    });
  },

  initChartSize() {
    const systemInfo =
      typeof wx.getWindowInfo === "function" ? wx.getWindowInfo() : wx.getSystemInfoSync();
    const chartWidth = Math.max(280, Math.floor(systemInfo.windowWidth - 44));

    this.setData({
      chartWidth,
    });
  },

  syncSettings() {
    const app = getApp();

    this.settings = app.getSettings ? app.getSettings() : app.globalData.settings;
  },

  onMetricTap(event) {
    const metric = event.currentTarget.dataset.metric;

    if (!metric || metric === this.data.metric) {
      return;
    }

    this.setData({
      metric,
      metricOptions: buildOptionState(METRIC_OPTIONS, metric),
    });

    this.loadHistory();
  },

  onRangeTap(event) {
    const range = event.currentTarget.dataset.range;

    if (!range || range === this.data.range) {
      return;
    }

    this.setData({
      range,
      rangeOptions: buildOptionState(RANGE_OPTIONS, range),
    });

    this.loadHistory();
  },

  onRefreshTap() {
    this.loadHistory();
  },

  loadHistory(options) {
    if (this.data.loading) {
      if (options && options.fromPullDown) {
        wx.stopPullDownRefresh();
      }
      return;
    }

    this.syncSettings();
    this.setData({
      loading: true,
      errorMessage: "",
    });

    callIotBridge("getThermalHistory", {
      metric: this.data.metric,
      range: this.data.range,
    })
      .then((data) => {
        this.rawHistoryData = data;
        this.applyHistoryData(data);
      })
      .catch((error) => {
        this.setData({
          errorMessage: error.message || "历史数据加载失败，请稍后重试。",
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

  applyHistoryData(rawData) {
    const viewModel = buildHistoryViewModel(rawData, this.settings);

    this.currentPoints = viewModel.points;
    this.setData({
      metric: viewModel.metric,
      range: viewModel.range,
      metricOptions: buildOptionState(METRIC_OPTIONS, viewModel.metric),
      rangeOptions: buildOptionState(RANGE_OPTIONS, viewModel.range),
      hasData: viewModel.points.length > 0,
      metricLabel: viewModel.metricLabel,
      rangeLabel: viewModel.rangeLabel,
      latestValueText: viewModel.latestValueText,
      minValueText: viewModel.minValueText,
      maxValueText: viewModel.maxValueText,
      sampleCountText: viewModel.sampleCountText,
      updatedAtText: viewModel.updatedAtText,
      unitLabel: viewModel.unitLabel,
      emptyText: viewModel.emptyText,
    });

    if (viewModel.points.length > 0) {
      wx.nextTick(() => {
        this.drawChart(viewModel);
      });
    }
  },

  drawChart(viewModel) {
    const points = viewModel.points;
    const chartWidth = this.data.chartWidth;
    const chartHeight = this.data.chartHeight;
    const padding = {
      top: 24,
      right: 18,
      bottom: 42,
      left: 52,
    };
    const plotWidth = chartWidth - padding.left - padding.right;
    const plotHeight = chartHeight - padding.top - padding.bottom;
    const ctx = wx.createCanvasContext("historyCanvas", this);
    const lineColor = METRIC_COLOR_MAP[viewModel.metric] || "#d86a38";
    const values = points.map((item) => item.value);
    let minValue = Math.min.apply(null, values);
    let maxValue = Math.max.apply(null, values);

    if (minValue === maxValue) {
      minValue -= 1;
      maxValue += 1;
    } else {
      const gap = maxValue - minValue;

      minValue -= gap * 0.12;
      maxValue += gap * 0.12;
    }

    const toX = (index) => {
      if (points.length === 1) {
        return padding.left + plotWidth / 2;
      }

      return padding.left + (plotWidth * index) / (points.length - 1);
    };
    const toY = (value) => padding.top + ((maxValue - value) / (maxValue - minValue)) * plotHeight;

    ctx.setFillStyle("#fffcf6");
    ctx.fillRect(0, 0, chartWidth, chartHeight);

    ctx.setStrokeStyle("rgba(123, 103, 88, 0.14)");
    ctx.setLineWidth(1);
    ctx.setFontSize(11);
    ctx.setFillStyle("#8a7a6b");

    for (let index = 0; index <= 4; index += 1) {
      const ratio = index / 4;
      const y = padding.top + plotHeight * ratio;
      const tickValue = maxValue - (maxValue - minValue) * ratio;

      ctx.beginPath();
      ctx.moveTo(padding.left, y);
      ctx.lineTo(chartWidth - padding.right, y);
      ctx.stroke();
      ctx.fillText(tickValue.toFixed(1), 4, y + 4);
    }

    const xIndexes = uniqueIndexes(
      [0, Math.round((points.length - 1) / 3), Math.round(((points.length - 1) * 2) / 3), points.length - 1],
      points.length
    );

    xIndexes.forEach((pointIndex) => {
      const x = toX(pointIndex);
      const label = formatAxisTime(points[pointIndex].timeMs, viewModel.range);

      ctx.beginPath();
      ctx.moveTo(x, chartHeight - padding.bottom);
      ctx.lineTo(x, chartHeight - padding.bottom + 8);
      ctx.stroke();
      ctx.fillText(label, x - 24, chartHeight - 10);
    });

    const gradient = ctx.createLinearGradient(0, padding.top, 0, chartHeight - padding.bottom);

    gradient.addColorStop(0, "rgba(216, 106, 56, 0.22)");
    gradient.addColorStop(1, "rgba(216, 106, 56, 0.02)");

    ctx.beginPath();
    ctx.moveTo(toX(0), chartHeight - padding.bottom);
    points.forEach((item, index) => {
      ctx.lineTo(toX(index), toY(item.value));
    });
    ctx.lineTo(toX(points.length - 1), chartHeight - padding.bottom);
    ctx.closePath();
    ctx.setFillStyle(gradient);
    ctx.fill();

    ctx.beginPath();
    points.forEach((item, index) => {
      const x = toX(index);
      const y = toY(item.value);

      if (index === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });
    ctx.setStrokeStyle(lineColor);
    ctx.setLineWidth(3);
    ctx.stroke();

    points.forEach((item, index) => {
      if (points.length > 20 && index !== 0 && index !== points.length - 1) {
        return;
      }

      const x = toX(index);
      const y = toY(item.value);

      ctx.beginPath();
      ctx.arc(x, y, index === points.length - 1 ? 4.5 : 3, 0, Math.PI * 2);
      ctx.setFillStyle(index === points.length - 1 ? "#fff4ee" : "#ffffff");
      ctx.fill();
      ctx.setStrokeStyle(lineColor);
      ctx.setLineWidth(index === points.length - 1 ? 3 : 2);
      ctx.stroke();
    });

    ctx.draw();
  },
});
