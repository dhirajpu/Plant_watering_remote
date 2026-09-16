# Firebase Hosting setup — Plant Life Care V2

The V2 dashboard is a static website located in `v2/`. `firebase.json` is configured so Firebase Hosting serves that directory as the site root.

## Architecture

```text
GitHub main
   |
   | GitHub Actions
   v
Firebase Hosting
   |
   v
Plant Life Care V2 dashboard
   |
   | Firebase Realtime Database REST API
   v
vernal-catfish-196407 /plantMonitor/v2
   ^
   |
  ESP32 V2
```

## One-time Firebase setup

1. Open the Firebase Console for the Firebase project used by the ESP32. The current dashboard configuration points to the Realtime Database project `vernal-catfish-196407`.
2. Open **Hosting** and initialize/enable Firebase Hosting for this project if it is not already enabled.
3. Open **Realtime Database → Rules** and review the rules before making the dashboard public. The dashboard can issue control commands and change plant configuration, so do not leave privileged write access publicly open.
4. Create a Firebase service account credential for GitHub Actions deployment. Do **not** commit the service-account JSON file to this repository.
5. In GitHub repository settings, add an Actions secret named:

   `FIREBASE_SERVICE_ACCOUNT`

   The secret value should be the complete service-account JSON credential.

## Deployment

After the secret is configured, every push to `main` that changes `v2/`, `firebase.json`, `.firebaserc`, or the Firebase workflow will deploy the V2 dashboard to Firebase Hosting automatically.

The workflow is:

`.github/workflows/firebase-hosting.yml`

The Firebase Hosting configuration is:

`firebase.json`

The selected Firebase project is:

`.firebaserc`

## Local deployment (optional)

If Firebase CLI is installed locally, from the repository root:

```bash
firebase login
firebase use vernal-catfish-196407
firebase deploy --only hosting
```

Because `firebase.json` uses `"public": "v2"`, the deployed site is the contents of `v2/`.

## GitHub Pages

GitHub Pages is no longer required for the V2 deployment. It can remain temporarily as a fallback while Firebase Hosting is verified. Once the Firebase URL is confirmed, the old GitHub Pages deployment can be disabled separately.

## Important security note

`v2/config.js` currently has an empty `authToken`, while the browser performs Firebase REST reads and writes. Firebase Realtime Database rules are therefore the actual protection boundary. Before using the public Firebase URL for pump control, configure authentication/rules so an unauthenticated visitor cannot write `/command` or `/config`.

Never put a Firebase Admin SDK private key or GitHub Actions service-account JSON into `v2/config.js` or any browser-served file.
