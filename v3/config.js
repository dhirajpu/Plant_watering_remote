window.PLANT_V3_CONFIG = {
  firebaseBaseUrl: "https://vernal-catfish-196407.firebaseio.com",
  // Firebase Console -> Project settings -> Your apps -> Web API Key.
  firebaseApiKey: "REPLACE_WITH_FIREBASE_WEB_API_KEY",
  deviceRoot: "/plantMonitor/v3/devices/REPLACE_WITH_DEVICE_ID",
  authToken: "",
  deviceId: "",
  staleTimeoutSec: 15,
  refreshMs: 3000
};
// Customer ownership:
// /plantMonitor/v3/users/{uid}/devices/{deviceId}
// /plantMonitor/v3/deviceOwners/{deviceId}
//
// Development milestone: the QR contains the Device ID and the first
// authenticated customer can claim an unclaimed device. Commercial production
// should replace this with a one-time enrollment token validated server-side.
// Never put customer passwords, service-account credentials, or database secrets here.