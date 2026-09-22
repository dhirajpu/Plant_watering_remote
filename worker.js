import { onRequest } from "./functions/api/[[path]].js";

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === "/superadmin") return env.ASSETS.fetch(new Request(new URL("/superadmin.html", request.url), request));
    if (url.pathname === "/api" || url.pathname.startsWith("/api/")) {
      const pathText = url.pathname.slice("/api".length).replace(/^\/+|\/+$/g, "");
      const path = pathText ? pathText.split("/") : [];
      return onRequest({ request, env, ctx, params: { path } });
    }
    return env.ASSETS.fetch(request);
  }
};
