const bleProvision = require("../../utils/bleProvision");

function buildLogLine(text) {
  const now = new Date();
  const hh = String(now.getHours()).padStart(2, "0");
  const mm = String(now.getMinutes()).padStart(2, "0");
  const ss = String(now.getSeconds()).padStart(2, "0");

  return `${hh}:${mm}:${ss} ${text}`;
}

function buildDefaultHelperText() {
  return "搜索附近设备后，选择你的设备并重新输入 Wi‑Fi 信息。推荐设备会优先显示。";
}

Page({
  data: {
    adapterReady: false,
    scanning: false,
    connecting: false,
    connected: false,
    commandBusy: false,
    deviceList: [],
    activeDeviceId: "",
    activeDeviceName: "",
    wifiSsid: "",
    wifiPassword: "",
    statusText: "未连接",
    helperText: buildDefaultHelperText(),
    errorMessage: "",
    logs: [],
    nearbyCount: 0,
    candidateCount: 0,
    scanElapsedSec: 0,
  },

  onLoad() {
    this.deviceMap = {};
    this.scanHintTimer = null;
    this.scanStartedAt = 0;

    this.valueChangeHandler = (result) => {
      if (!result || result.deviceId !== this.data.activeDeviceId) {
        return;
      }

      const message = bleProvision.decodeStatusMessage(result.value);

      this.handleProvisionStatus(message);
    };
    this.connectionChangeHandler = (result) => {
      if (!result || result.deviceId !== this.data.activeDeviceId || result.connected) {
        return;
      }

      this.appendLog("蓝牙连接已断开");
      this.setData({
        connected: false,
        activeDeviceId: "",
        activeDeviceName: "",
        statusText: "连接已断开",
        helperText: "设备已经断开连接。你可以重新搜索，或直接再次点击列表里的设备。",
      });
    };

    wx.onBLECharacteristicValueChange(this.valueChangeHandler);
    wx.onBLEConnectionStateChange(this.connectionChangeHandler);
  },

  onUnload() {
    this.cleanupBle();
  },

  async cleanupBle() {
    this.stopScanHintTimer();
    if (typeof wx.offBLECharacteristicValueChange === "function" && this.valueChangeHandler) {
      wx.offBLECharacteristicValueChange(this.valueChangeHandler);
    }
    if (typeof wx.offBLEConnectionStateChange === "function" && this.connectionChangeHandler) {
      wx.offBLEConnectionStateChange(this.connectionChangeHandler);
    }
    await bleProvision.stopDiscovery();
    if (this.connection) {
      await bleProvision.disconnectDevice(this.connection.deviceId);
      this.connection = null;
    }
    await bleProvision.closeAdapter();
  },

  appendLog(text) {
    const logs = [buildLogLine(text), ...(this.data.logs || [])].slice(0, 12);

    this.setData({ logs });
  },

  stopScanHintTimer() {
    if (this.scanHintTimer) {
      clearInterval(this.scanHintTimer);
      this.scanHintTimer = null;
    }
  },

  updateScanHint() {
    const nearbyCount = Object.keys(this.deviceMap).length;
    const candidateCount = Object.values(this.deviceMap).filter((item) => item.matches).length;
    const elapsedSec = this.scanStartedAt ? Math.max(1, Math.floor((Date.now() - this.scanStartedAt) / 1000)) : 0;
    let helperText = buildDefaultHelperText();

    if (this.data.scanning) {
      if (nearbyCount === 0) {
        helperText = `正在搜索（${elapsedSec}s），暂未发现附近设备。请确认手机蓝牙、定位权限以及设备已通电。`;
      } else if (candidateCount === 0) {
        helperText = `正在搜索（${elapsedSec}s），已发现 ${nearbyCount} 台附近设备。你也可以手动点击列表尝试连接。`;
      } else {
        helperText = `正在搜索（${elapsedSec}s），已发现 ${nearbyCount} 台附近设备，其中 ${candidateCount} 台更像是你的设备。`;
      }
    } else if (!this.data.connected && nearbyCount > 0) {
      helperText = `搜索已停止，当前列表里有 ${nearbyCount} 台附近设备，其中 ${candidateCount} 台为推荐设备。`;
    }

    this.setData({
      nearbyCount,
      candidateCount,
      scanElapsedSec: elapsedSec,
      helperText,
    });
  },

  mergeDevices(devices) {
    devices.forEach((item) => {
      this.deviceMap[item.deviceId] = {
        ...(this.deviceMap[item.deviceId] || {}),
        ...item,
      };
    });

    const deviceList = Object.values(this.deviceMap).sort((left, right) => {
      if (left.matches !== right.matches) {
        return left.matches ? -1 : 1;
      }
      if ((left.name || "") !== (right.name || "")) {
        return (left.name || "").localeCompare(right.name || "");
      }
      return right.rssi - left.rssi;
    });

    this.setData({ deviceList });
    this.updateScanHint();
  },

  async ensureAdapterReady() {
    await bleProvision.openAdapter();
    this.setData({
      adapterReady: true,
      errorMessage: "",
    });
  },

  async onStartScanTap() {
    try {
      await this.ensureAdapterReady();
      this.stopScanHintTimer();
      this.deviceMap = {};
      this.scanStartedAt = Date.now();
      this.setData({
        scanning: true,
        deviceList: [],
        nearbyCount: 0,
        candidateCount: 0,
        scanElapsedSec: 0,
        statusText: "正在扫描",
        errorMessage: "",
      });
      this.updateScanHint();
      this.scanHintTimer = setInterval(() => {
        this.updateScanHint();
      }, 1000);

      await bleProvision.startDiscovery((devices) => {
        this.mergeDevices(devices);
      });
      this.appendLog("开始搜索附近设备");
    } catch (error) {
      this.stopScanHintTimer();
      this.setData({
        scanning: false,
        statusText: "搜索失败",
        errorMessage: error.message || "蓝牙不可用，请检查系统蓝牙、定位权限和微信授权。",
        helperText: "如果手机蓝牙权限未打开，小程序通常会一直显示搜索中，但找不到任何设备。",
      });
    }
  },

  async onStopScanTap() {
    this.stopScanHintTimer();
    await bleProvision.stopDiscovery();
    this.setData({
      scanning: false,
      statusText: this.data.connected ? this.data.statusText : "搜索已停止",
    });
    this.updateScanHint();
  },

  async onDeviceTap(event) {
    const { deviceId } = event.currentTarget.dataset;
    const device = this.deviceMap[deviceId];

    if (!deviceId || !device || this.data.connecting) {
      return;
    }

    try {
      this.setData({
        connecting: true,
        errorMessage: "",
        helperText: `正在连接 ${device.name}，如果它不是可联网设备，稍后会给出明确提示。`,
      });
      await bleProvision.stopDiscovery();
      this.stopScanHintTimer();
      this.connection = await bleProvision.connectDevice(deviceId);
      this.setData({
        scanning: false,
        connected: true,
        activeDeviceId: deviceId,
        activeDeviceName: device.name,
        statusText: "已连接，等待设备状态",
        helperText: "连接成功。设备会实时回传联网状态。",
      });
      this.appendLog(`已连接 ${device.name}`);
    } catch (error) {
      this.setData({
        scanning: false,
        statusText: "连接失败",
        errorMessage: error.message || "连接设备失败",
        helperText: "如果能看到设备但连接失败，通常说明它不是目标设备，或当前不支持重新联网。",
      });
      this.appendLog(`连接失败: ${device.name}`);
    } finally {
      this.setData({
        connecting: false,
      });
      this.updateScanHint();
    }
  },

  parseReadyStatus(message) {
    const [, configured = "0", connected = "0", waiting = "0"] = message.split("|");
    const pieces = [];

    pieces.push(configured === "1" ? "已保存 Wi‑Fi" : "未保存 Wi‑Fi");
    pieces.push(connected === "1" ? "已连网" : "未连网");
    pieces.push(waiting === "1" ? "正在等待联网结果" : "空闲");
    return pieces.join(" / ");
  },

  handleProvisionStatus(message) {
    let statusText = message;

    if (!message) {
      return;
    }

    if (message.startsWith("READY|")) {
      statusText = this.parseReadyStatus(message);
    } else if (message === "SAVING") {
      statusText = "正在保存 Wi‑Fi 配置";
    } else if (message === "SAVED") {
      statusText = "Wi‑Fi 配置已保存";
    } else if (message === "CONNECTING") {
      statusText = "设备正在连接 Wi‑Fi";
    } else if (message === "CONNECTED") {
      statusText = "设备已连上 Wi‑Fi";
    } else if (message.startsWith("DISC|")) {
      statusText = `Wi‑Fi 断开，原因 ${message.slice(5)}`;
    } else if (message === "CLEARED") {
      statusText = "Wi‑Fi 配置已清空";
    } else if (message.startsWith("ERR|")) {
      statusText = `设备返回错误：${message.slice(4)}`;
    }

    this.appendLog(`设备: ${message}`);
    this.setData({
      statusText,
      helperText: "状态由设备实时回传，小程序这里不做本地猜测。",
    });
  },

  onSsidInput(event) {
    this.setData({
      wifiSsid: event.detail.value,
    });
  },

  onPasswordInput(event) {
    this.setData({
      wifiPassword: event.detail.value,
    });
  },

  async onSendProvisionTap() {
    const ssid = (this.data.wifiSsid || "").trim();
    const password = this.data.wifiPassword || "";

    if (!this.connection || !this.data.connected) {
      this.setData({
        errorMessage: "请先连接设备，再下发 Wi‑Fi 配置。",
      });
      return;
    }
    if (!ssid || !password) {
      this.setData({
        errorMessage: "SSID 和密码都不能为空。",
      });
      return;
    }

    try {
      this.setData({
        commandBusy: true,
        errorMessage: "",
      });
      this.appendLog(`发送 Wi‑Fi 配置: ${ssid}`);
      await bleProvision.sendCommand(this.connection, {
        cmd: "set_wifi",
        ssid,
        password,
      });
    } catch (error) {
      this.setData({
        errorMessage: error.message || "发送 Wi‑Fi 配置失败。",
      });
    } finally {
      this.setData({
        commandBusy: false,
      });
    }
  },

  async onQueryStatusTap() {
    if (!this.connection || !this.data.connected) {
      return;
    }

    try {
      this.setData({
        commandBusy: true,
        errorMessage: "",
      });
      this.appendLog("请求设备上报当前状态");
      await bleProvision.sendCommand(this.connection, {
        cmd: "status",
      });
    } catch (error) {
      this.setData({
        errorMessage: error.message || "请求状态失败。",
      });
    } finally {
      this.setData({
        commandBusy: false,
      });
    }
  },

  async onClearWifiTap() {
    if (!this.connection || !this.data.connected) {
      return;
    }

    try {
      this.setData({
        commandBusy: true,
        errorMessage: "",
      });
      this.appendLog("发送清空 Wi‑Fi 配置命令");
      await bleProvision.sendCommand(this.connection, {
        cmd: "clear_wifi",
      });
    } catch (error) {
      this.setData({
        errorMessage: error.message || "清空 Wi‑Fi 配置失败。",
      });
    } finally {
      this.setData({
        commandBusy: false,
      });
    }
  },

  async onDisconnectTap() {
    if (!this.connection) {
      return;
    }

    await bleProvision.disconnectDevice(this.connection.deviceId);
    this.connection = null;
    this.setData({
      connected: false,
      activeDeviceId: "",
      activeDeviceName: "",
      statusText: "已主动断开",
      helperText: "如果需要再次下发配置，可以重新连接设备。",
    });
  },
});
