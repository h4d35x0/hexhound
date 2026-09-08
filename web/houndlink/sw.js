/* HoundLink service worker.

   Purpose: make the app work with no network at all, which is the honest state
   for a tool whose whole job is talking to a USB device offline. It is NOT here
   to sync anything, because there is nothing to sync with.

   Two properties this file must never lose:

   1. SCOPE. It is registered from web/houndlink/ with scope "./", so it can only
      ever see requests under /houndlink/. The existing ESP Web Tools flasher at
      the site root is outside this scope and cannot be intercepted, cached or
      broken by anything here. That separation is deliberate: the flasher must
      keep working exactly as it did before this app existed.

   2. NO NETWORK EGRESS. Nothing here fetches a third-party origin, and the
      cross-origin case is explicitly passed straight through rather than being
      cached or rewritten. Combined with the page's Content Security Policy,
      "HoundLink never uploads anything" stays true even for requests that would
      go through the worker.
*/

const CACHE = "houndlink-v0.1.0";

// The whole app. There is no lazy loading and no code splitting, so this list is
// the complete set of files the app needs to run.
const SHELL = [
  "./",
  "./index.html",
  "./manifest.webmanifest",
  "./icon.svg",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE)
      .then((cache) => cache.addAll(SHELL))
      // Take over promptly so a reload after an update gets the new app rather
      // than serving the previous version until every tab is closed.
      .then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((names) => Promise.all(
        names.filter((n) => n.startsWith("houndlink-") && n !== CACHE)
             .map((n) => caches.delete(n))
      ))
      .then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (event) => {
  const req = event.request;

  // Only ever handle same-origin GETs. Anything else, including the
  // cross-origin case that should never happen, is passed through untouched.
  if (req.method !== "GET") return;
  if (new URL(req.url).origin !== self.location.origin) return;

  // Network first, falling back to cache.
  //
  // Cache-first would be faster, but it makes local development quietly
  // confusing: you edit index.html, reload, and see the old page with no
  // indication why. For an app this small the network round trip on localhost is
  // irrelevant, and being able to trust what you are looking at is worth more.
  // The cache still does its real job, which is working with no network at all.
  event.respondWith(
    fetch(req)
      .then((res) => {
        if (res && res.ok) {
          const copy = res.clone();
          caches.open(CACHE).then((cache) => cache.put(req, copy)).catch(() => {});
        }
        return res;
      })
      .catch(() => caches.match(req).then((hit) => hit || caches.match("./index.html")))
  );
});
