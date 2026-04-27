const SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const TX_CHARACTERISTIC_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
const RX_CHARACTERISTIC_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const DEVICE_NAME_KEYWORD = "REDPIC";
const WRITE_CHUNK_SIZE = 20;

let currentDiscoveryListener = null;

function promisifyWxCall(method, options = {}) {
  return new Promise((resolve, reject) => {
    method({
      ...options,
      success: resolve,
      fail: reject,
    });
  });
}

function uppercaseUuid(uuid) {
  return (uuid || "").toUpperCase();
}

function normalizeName(device) {
  const rawName = device.localName || device.name || "";
  const trimmed = typeof rawName === "string" ? rawName.trim() : "";

  return trimmed || "未命名设备";
}

function buildMatchText(matchesName, matchesService) {
  if (matchesName && matchesService) {
    return "更像是你的设备，建议优先尝试连接";
  }
  if (matchesService) {
    return "已识别到可联网服务";
  }
  if (matchesName) {
    return "名称与目标设备相近";
  }
  return "也可以尝试连接，若不匹配会自动提示";
}

function buildSignalText(rssi) {
  if (typeof rssi !== "number" || !Number.isFinite(rssi)) {
    return "信号未知";
  }

  if (rssi >= -65) {
    return "信号良好";
  }

  if (rssi >= -80) {
    return "信号一般";
  }

  return "信号较弱";
}

function normalizeDevice(device) {
  const name = normalizeName(device);
  const advertisServiceUUIDs = (device.advertisServiceUUIDs || []).map(uppercaseUuid);
  const matchesName = uppercaseUuid(name).includes(DEVICE_NAME_KEYWORD);
  const matchesService = advertisServiceUUIDs.includes(SERVICE_UUID);
  const matches = matchesName || matchesService;
  const rssi = typeof device.RSSI === "number" ? device.RSSI : -999;

  return {
    deviceId: device.deviceId,
    name,
    localName: device.localName || "",
    rssi,
    advertisServiceUUIDs,
    matches,
    matchesName,
    matchesService,
    matchText: buildMatchText(matchesName, matchesService),
    badgeText: matches ? "推荐连接" : "可尝试连接",
    signalText: buildSignalText(rssi),
  };
}

function encodeUtf8(text) {
  const bytes = [];
  const value = String(text || "");

  for (let i = 0; i < value.length; i += 1) {
    let codePoint = value.charCodeAt(i);

    if (codePoint >= 0xd800 && codePoint <= 0xdbff && i + 1 < value.length) {
      const low = value.charCodeAt(i + 1);

      if (low >= 0xdc00 && low <= 0xdfff) {
        codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
        i += 1;
      }
    }

    if (codePoint <= 0x7f) {
      bytes.push(codePoint);
    } else if (codePoint <= 0x7ff) {
      bytes.push(0xc0 | (codePoint >> 6));
      bytes.push(0x80 | (codePoint & 0x3f));
    } else if (codePoint <= 0xffff) {
      bytes.push(0xe0 | (codePoint >> 12));
      bytes.push(0x80 | ((codePoint >> 6) & 0x3f));
      bytes.push(0x80 | (codePoint & 0x3f));
    } else {
      bytes.push(0xf0 | (codePoint >> 18));
      bytes.push(0x80 | ((codePoint >> 12) & 0x3f));
      bytes.push(0x80 | ((codePoint >> 6) & 0x3f));
      bytes.push(0x80 | (codePoint & 0x3f));
    }
  }

  return Uint8Array.from(bytes).buffer;
}

function decodeAscii(buffer) {
  const view = new Uint8Array(buffer);
  let result = "";

  for (let i = 0; i < view.length; i += 1) {
    if (view[i] === 0) {
      break;
    }
    result += String.fromCharCode(view[i]);
  }

  return result;
}

function delay(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

async function openAdapter() {
  await promisifyWxCall(wx.openBluetoothAdapter);
}

async function closeAdapter() {
  if (typeof wx.offBluetoothDeviceFound === "function" && currentDiscoveryListener) {
    wx.offBluetoothDeviceFound(currentDiscoveryListener);
  }
  currentDiscoveryListener = null;
  await promisifyWxCall(wx.closeBluetoothAdapter).catch(() => {});
}

async function emitExistingDevices(onFound) {
  const result = await promisifyWxCall(wx.getBluetoothDevices).catch(() => null);
  const devices = result && Array.isArray(result.devices) ? result.devices.map(normalizeDevice) : [];

  if (devices.length && typeof onFound === "function") {
    onFound(devices);
  }
}

async function startDiscovery(onFound) {
  await openAdapter();

  if (typeof wx.offBluetoothDeviceFound === "function" && currentDiscoveryListener) {
    wx.offBluetoothDeviceFound(currentDiscoveryListener);
  }

  currentDiscoveryListener = (payload) => {
    const devices = (payload.devices || [])
      .map(normalizeDevice)
      .filter((item) => !!item.deviceId);

    if (devices.length && typeof onFound === "function") {
      onFound(devices);
    }
  };

  wx.onBluetoothDeviceFound(currentDiscoveryListener);
  await promisifyWxCall(wx.startBluetoothDevicesDiscovery, {
    allowDuplicatesKey: false,
    interval: 0,
  });
  await emitExistingDevices(onFound);
}

async function stopDiscovery() {
  await promisifyWxCall(wx.stopBluetoothDevicesDiscovery).catch(() => {});
  if (typeof wx.offBluetoothDeviceFound === "function" && currentDiscoveryListener) {
    wx.offBluetoothDeviceFound(currentDiscoveryListener);
  }
  currentDiscoveryListener = null;
}

async function connectDevice(deviceId) {
  let servicesResult = null;
  let service = null;
  let characteristicsResult = null;
  let txCharacteristic = null;
  let rxCharacteristic = null;
  let availableServices = [];

  await promisifyWxCall(wx.createBLEConnection, {
    deviceId,
    timeout: 10000,
  });

  try {
    servicesResult = await promisifyWxCall(wx.getBLEDeviceServices, { deviceId });
    availableServices = (servicesResult.services || []).map((item) => uppercaseUuid(item.uuid));
    service = (servicesResult.services || []).find(
      (item) => uppercaseUuid(item.uuid) === SERVICE_UUID
    );

    if (!service) {
      throw new Error(
        availableServices.length
          ? "该设备已连接，但暂不支持当前联网方式，请换一个设备重试。"
          : "该设备已连接，但暂时无法继续联网配置。"
      );
    }

    characteristicsResult = await promisifyWxCall(wx.getBLEDeviceCharacteristics, {
      deviceId,
      serviceId: service.uuid,
    });
    txCharacteristic = (characteristicsResult.characteristics || []).find(
      (item) => uppercaseUuid(item.uuid) === TX_CHARACTERISTIC_UUID
    );
    rxCharacteristic = (characteristicsResult.characteristics || []).find(
      (item) => uppercaseUuid(item.uuid) === RX_CHARACTERISTIC_UUID
    );

    if (!txCharacteristic || !rxCharacteristic) {
      throw new Error("该设备暂不支持当前联网方式，请换一个设备重试。");
    }

    await promisifyWxCall(wx.notifyBLECharacteristicValueChange, {
      deviceId,
      serviceId: service.uuid,
      characteristicId: txCharacteristic.uuid,
      state: true,
    });

    return {
      deviceId,
      serviceId: service.uuid,
      txCharacteristicId: txCharacteristic.uuid,
      rxCharacteristicId: rxCharacteristic.uuid,
    };
  } catch (error) {
    await disconnectDevice(deviceId);
    throw error;
  }
}

async function disconnectDevice(deviceId) {
  if (!deviceId) {
    return;
  }

  await promisifyWxCall(wx.closeBLEConnection, {
    deviceId,
  }).catch(() => {});
}

async function sendCommand(connection, command) {
  const payload = `${JSON.stringify(command)}\n`;
  const bytes = new Uint8Array(encodeUtf8(payload));

  for (let offset = 0; offset < bytes.length; offset += WRITE_CHUNK_SIZE) {
    const chunk = bytes.slice(offset, offset + WRITE_CHUNK_SIZE);

    await promisifyWxCall(wx.writeBLECharacteristicValue, {
      deviceId: connection.deviceId,
      serviceId: connection.serviceId,
      characteristicId: connection.rxCharacteristicId,
      value: chunk.buffer,
    });
    await delay(35);
  }
}

module.exports = {
  SERVICE_UUID,
  TX_CHARACTERISTIC_UUID,
  RX_CHARACTERISTIC_UUID,
  normalizeDevice,
  openAdapter,
  closeAdapter,
  startDiscovery,
  stopDiscovery,
  connectDevice,
  disconnectDevice,
  sendCommand,
  decodeStatusMessage: decodeAscii,
};
