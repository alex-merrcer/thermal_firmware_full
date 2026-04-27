const bleProvision = require("../../utils/bleProvision");

function buildLogLine(text) {
  const now = new Date();
  const hh = String(now.getHours()).padStart(2, "0");
  const mm = String(now.getMinutes()).padStart(2, "0");
  const ss = String(now.getSeconds()).padStart(2, "0");

  return `${hh}:${mm}:${ss} ${text}`;
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
    helperText: "扫描附近的 RedPic1 Setup 设备，写入 Wi-Fi 后观察连接结果。",
    errorMessage: "",
    logs: [],
  },

  onLoad() {
    this.deviceMap = {};

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
      });
    };

    wx.onBLECharacteristicValueChange(this.valueChangeHandler);
    wx.onBLEConnectionStateChange(this.connectionChangeHandler);
  },

  onUnload() {
    this.cleanupBle();
  },

  async cleanupBle() {
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

  mergeDevices(devices) {
    devices.forEach((item) => {
      this.deviceMap[item.deviceId] = item;
    });

    const deviceList = Object.values(this.deviceMap).sort((left, right) => {
      if (left.name === right.name) {
        return right.rssi - left.rssi;
      }
      return left.name.localeCompare(right.name);
    });

    this.setData({ deviceList });
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
      this.deviceMap = {};
      this.setData({
        scanning: true,
        deviceList: [],
        helperText: "正在扫描支持 BLE 配网的设备…",
      });
      await bleProvision.startDiscovery((devices) => {
        this.mergeDevices(devices);
      });
      this.appendLog("开始扫描 BLE 配网设备");
    } catch (error) {
      this.setData({
        scanning: false,
        errorMessage: error.message || "蓝牙不可用，请检查系统蓝牙权限",
      });
    }
  },

  async onStopScanTap() {
    await bleProvision.stopDiscovery();
    this.setData({
      scanning: false,
      helperText: "扫描已停止，可以选择已发现的设备进行连接。",
    });
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
        helperText: `正在连接 ${device.name}…`,
      });
      await bleProvision.stopDiscovery();
      this.connection = await bleProvision.connectDevice(deviceId);
      this.setData({
        scanning: false,
        connected: true,
        activeDeviceId: deviceId,
        activeDeviceName: device.name,
        statusText: "已连接，等待设备状态",
        helperText: "连接成功，设备会通过通知特征返回当前配网状态。",
      });
      this.appendLog(`已连接 ${device.name}`);
    } catch (error) {
      this.setData({
        errorMessage: error.message || "连接设备失败",
        helperText: "连接失败后可以重新扫描或再次尝试连接。",
      });
    } finally {
      this.setData({
        connecting: false,
      });
    }
  },

  parseReadyStatus(message) {
    const [, configured = "0", connected = "0", waiting = "0"] = message.split("|");
    const pieces = [];

    pieces.push(configured === "1" ? "已保存 Wi-Fi" : "未保存 Wi-Fi");
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
      statusText = "正在保存 Wi-Fi 配置";
    } else if (message === "SAVED") {
      statusText = "Wi-Fi 配置已保存";
    } else if (message === "CONNECTING") {
      statusText = "设备正在连接 Wi-Fi";
    } else if (message === "CONNECTED") {
      statusText = "设备已连上 Wi-Fi";
    } else if (message.startsWith("DISC|")) {
      statusText = `Wi-Fi 断开：${message.slice(5)}`;
    } else if (message === "CLEARED") {
      statusText = "Wi-Fi 配置已清空";
    } else if (message.startsWith("ERR|")) {
      statusText = `设备返回错误：${message.slice(4)}`;
    }

    this.appendLog(`设备: ${message}`);
    this.setData({
      statusText,
      helperText: "状态由 ESP32 通过 BLE 通知实时回传，小程序这里不做本地猜测。",
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
        errorMessage: "请先连接设备再下发 Wi-Fi 配置",
      });
      return;
    }
    if (!ssid || !password) {
      this.setData({
        errorMessage: "SSID 和密码都不能为空",
      });
      return;
    }

    try {
      this.setData({
        commandBusy: true,
        errorMessage: "",
      });
      this.appendLog(`发送 Wi-Fi 配置: ${ssid}`);
      await bleProvision.sendCommand(this.connection, {
        cmd: "set_wifi",
        ssid,
        password,
      });
    } catch (error) {
      this.setData({
        errorMessage: error.message || "发送 Wi-Fi 配置失败",
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
        errorMessage: error.message || "请求状态失败",
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
      this.appendLog("发送清空 Wi-Fi 配置命令");
      await bleProvision.sendCommand(this.connection, {
        cmd: "clear_wifi",
      });
    } catch (error) {
      this.setData({
        errorMessage: error.message || "清空 Wi-Fi 配置失败",
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
      helperText: "如需再次下发配置，可以重新连接设备。",
    });
  },
});
