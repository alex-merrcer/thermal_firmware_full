# Thermal Mini Program

This mini program is the smallest workable architecture for your project:

`STM32 -> ESP32 -> Alibaba Cloud IoT -> WeChat cloud function -> WeChat mini program`

There is no traditional server in this version.  
The cloud function acts as the backend and queries the latest device property snapshot from Alibaba Cloud IoT.

## What Is Included

- `miniprogram/pages/index`
  A thermal dashboard page that shows min, max, and center temperature.
- `miniprogram/pages/weather`
  A weather page backed by a WeChat cloud function.
- `cloudfunctions/iotBridge`
  A cloud function that signs and calls Alibaba Cloud IoT OpenAPI.

## Files You Need To Fill

### 1. Mini program cloud environment

Edit `miniprogram/app.js`:

```js
envId: "YOUR_CLOUD_ENV_ID"
```

### 2. Alibaba Cloud IoT configuration

Edit `cloudfunctions/iotBridge/config.js`:

```js
accessKeyId: "YOUR_ACCESS_KEY_ID",
accessKeySecret: "YOUR_ACCESS_KEY_SECRET",
regionId: "cn-shanghai",
endpoint: "",
iotInstanceId: "",
productKey: "YOUR_PRODUCT_KEY",
deviceName: "YOUR_DEVICE_NAME",
propertyIdentifiers: {
  minTemp: "MinTemp",
  maxTemp: "MaxTemp",
  centerTemp: "CenterTemp",
}
```

Notes:

- `endpoint` can stay empty for many public-region cases.
- If your Alibaba Cloud console gives you a dedicated IoT endpoint, fill it here.
- If your IoT instance has an `IotInstanceId`, fill it.
- `MinTemp / MaxTemp / CenterTemp` must match the TSL property identifiers on Alibaba Cloud IoT.

### 3. Weather configuration

Edit `cloudfunctions/iotBridge/config.js`:

```js
WEATHER_CONFIG: {
  baseUrl: "https://api.seniverse.com",
  apiKey: "YOUR_WEATHER_API_KEY",
  defaultLocation: "Shanghai",
}
```

Notes:

- the weather page uses the cloud function, not the ESP32 runtime
- if `apiKey` is not filled, the weather page will show a clear config error

## Alibaba Cloud IoT Side

In your product TSL, create these properties:

- `MinTemp`
- `MaxTemp`
- `CenterTemp`

Recommended type: `float`

The ESP32 side should already be reporting these properties to Alibaba Cloud IoT.

## Deploy Steps

1. Open `miniprogram-1` in WeChat DevTools.
2. Create or select a cloud environment.
3. Fill `miniprogram/app.js`.
4. Fill `cloudfunctions/iotBridge/config.js`.
5. Right-click `cloudfunctions/iotBridge` and choose upload and deploy.
6. Open the mini program home page.
7. Tap `Refresh`.

## What You Should See

- The page shows:
  - min temperature
  - max temperature
  - center temperature
  - latest property update time
  - cloud request time
- If configuration is incomplete, the page will show a clear error card.

## Current Cloud Function Logic

The cloud function currently calls Alibaba Cloud IoT `QueryDevicePropertyStatus` to get the latest property snapshot.

That means:

- this version is good for a current-value dashboard
- it is not yet a history chart
- later you can extend it with history, alarms, or polling

## Suggested Next Step

After this page works, the next smallest upgrade is:

- add auto refresh every 5 to 10 seconds
- add online/offline status
- add a historical trend chart for the center temperature
