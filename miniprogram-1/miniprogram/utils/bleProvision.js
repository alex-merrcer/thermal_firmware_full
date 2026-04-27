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

function normalizeDevice(device) {
  const name = device.localName || device.name || "未命名设备";
  const advertisServiceUUIDs = (device.advertisServiceUUIDs || []).map(uppercaseUuid);
  const matches =
    uppercaseUuid(name).includes(DEVICE_NAME_KEYWORD) ||
    advertisServiceUUIDs.includes(SERVICE_UUID);

  return {
    deviceId: device.deviceId,
    name,
    localName: device.localName || "",
    rssi: typeof device.RSSI === "number" ? device.RSSI : -999,
    advertisServiceUUIDs,
    matches,
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

async function startDiscovery(onFound) {
  await openAdapter();

  if (typeof wx.offBluetoothDeviceFound === "function" && currentDiscoveryListener) {
    wx.offBluetoothDeviceFound(currentDiscoveryListener);
  }

  currentDiscoveryListener = (payload) => {
    const devices = (payload.devices || [])
      .map(normalizeDevice)
      .filter((item) => item.matches);

    if (devices.length && typeof onFound === "function") {
      onFound(devices);
    }
  };

  wx.onBluetoothDeviceFound(currentDiscoveryListener);
  await promisifyWxCall(wx.startBluetoothDevicesDiscovery, {
    allowDuplicatesKey: false,
    services: [SERVICE_UUID],
  });
}

async function stopDiscovery() {
  await promisifyWxCall(wx.stopBluetoothDevicesDiscovery).catch(() => {});
  if (typeof wx.offBluetoothDeviceFound === "function" && currentDiscoveryListener) {
    wx.offBluetoothDeviceFound(currentDiscoveryListener);
  }
  currentDiscoveryListener = null;
}

async function connectDevice(deviceId) {
  const servicesResult = await promisifyWxCall(wx.createBLEConnection, {
    deviceId,
    timeout: 10000,
  }).then(() => promisifyWxCall(wx.getBLEDeviceServices, { deviceId }));
  const service = (servicesResult.services || []).find(
    (item) => uppercaseUuid(item.uuid) === SERVICE_UUID
  );

  if (!service) {
    throw new Error("设备未暴露配网服务");
  }

  const characteristicsResult = await promisifyWxCall(wx.getBLEDeviceCharacteristics, {
    deviceId,
    serviceId: service.uuid,
  });
  const characteristics = characteristicsResult.characteristics || [];
  const txCharacteristic = characteristics.find(
    (item) => uppercaseUuid(item.uuid) === TX_CHARACTERISTIC_UUID
  );
  const rxCharacteristic = characteristics.find(
    (item) => uppercaseUuid(item.uuid) === RX_CHARACTERISTIC_UUID
  );

  if (!txCharacteristic || !rxCharacteristic) {
    throw new Error("设备配网特征缺失");
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
