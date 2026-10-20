// The one place that says what this browser calls itself on the wire and to scripts: the HTTP User-Agent header, navigator.userAgent /
// appVersion (wjs.c native W.userAgent) and the Accept-Language header (navigator.languages in the prelude is the same list).
// Honest identity: no Chrome/Firefox claim, no client hints. No trailing "Safari/537.36" token on purpose: YouTube answers a UA that
// carries it with its polyfilled legacy page (webcomponents-sd, custom-elements-es5-adapter: slower and half-hydrated here), and a UA without
// it with the native custom-elements page that this engine renders fully (measured 2026-10-16: page height 1225 px vs 3834).
#ifndef WEB_WUA_H
#define WEB_WUA_H
#define OKAI_USER_AGENT "Mozilla/5.0 (X11; okernel i686) AppleWebKit/537.36 (KHTML, like Gecko) okai/0.6"
#define OKAI_ACCEPT_LANGUAGE "en-US,en;q=0.9"
#endif
