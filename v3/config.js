window.PLANT_V3_CONFIG = {
  firebaseBaseUrl: "https://vernal-catfish-196407.firebaseio.com",
  deviceRoot: "/plantMonitor/v3/devices/REPLACE_WITH_DEVICE_ID",
  authToken: "",
  staleTimeoutSec: 15,
  refreshMs: 3000
};

// V3 is intended to be a customer-facing application.
// The production version will resolve deviceRoot from the authenticated
// customer account/device claim instead of hard-coding a device ID.
// Do not put customer passwords or long-lived secrets in this file.
