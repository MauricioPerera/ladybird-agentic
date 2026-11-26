# YouTube Clean & Adblock Extension

Combines:
1. MV3 `declarative_net_request` (`rules.json`) + `ladybird.filter_lists` (`filters.txt`) compiled straight into `Web::ContentBlocker` (network blocking + cosmetic zero-FOUC CSS).
2. `content.css` + `content.js` with a `MutationObserver` and `yt-navigate-finish` listener to remove dynamically injected Shorts shelves/cards and redirect `/shorts/<id>` to `/watch?v=<id>`.

Load `D:\ladybird\Tools\extensions\youtube-clean-and-adblock` in `about:extensions` -> **Load Unpacked** or via MCP `install_unpacked_extension`.
