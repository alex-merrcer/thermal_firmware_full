function buildFriendlyErrorMessage(error) {
  if (!error) {
    return "请求失败，请稍后重试。";
  }

  if (error.errorCode === "CONFIG_MISSING") {
    return "服务配置尚未完成，请稍后再试。";
  }

  if (error.errorCode === "INVALID_NICKNAME") {
    return error.message || "设备名称格式不符合要求。";
  }

  if (typeof error.errMsg === "string") {
    if (error.errMsg.indexOf("FunctionName parameter could not be found") >= 0) {
      return "服务暂未准备好，请稍后再试。";
    }
    if (error.errMsg.indexOf("Environment not found") >= 0) {
      return "当前服务暂不可用，请稍后再试。";
    }

    return error.errMsg;
  }

  if (typeof error.message === "string") {
    return error.message;
  }

  return "请求失败，请稍后重试。";
}

function callIotBridge(action, data) {
  const app = getApp();

  if (!app.globalData.cloudReady) {
    return Promise.reject(new Error("服务暂未准备好，请稍后再试。"));
  }

  return wx.cloud
    .callFunction({
      name: app.globalData.cloudFunctionName,
      data: {
        action,
        ...(data || {}),
      },
    })
    .then((response) => {
      const result = response && response.result ? response.result : null;

      if (!result || result.success !== true) {
        const message = buildFriendlyErrorMessage(result);
        app.setLastBridgeError(message);
        throw Object.assign(new Error(message), result || {});
      }

      app.clearLastBridgeError();
      return result.data;
    })
    .catch((error) => {
      const message = buildFriendlyErrorMessage(error);
      app.setLastBridgeError(message);
      throw Object.assign(new Error(message), error || {});
    });
}

module.exports = {
  buildFriendlyErrorMessage,
  callIotBridge,
};
