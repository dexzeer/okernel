# Filter list provenance

These files are third-party **data** (filter lists), shipped inside the
`ablists.txt` boot module. They are the only third-party content in this
repository besides the approved exceptions in `AGENTS.md` (fonts, Realtek
firmware, AAC Huffman tables, CELT from RFC 6716). The filter engine
(`src/ablock.c`) is our own code.

| File | Source | License |
|---|---|---|
| `easylist.txt` | https://easylist.to/easylist/easylist.txt | CC BY 3.0 (https://easylist.to/pages/licence.html) |
| `easyprivacy.txt` | https://easylist.to/easylist/easyprivacy.txt | CC BY 3.0 (https://easylist.to/pages/licence.html) |

EasyList and EasyPrivacy are © the EasyList authors and are licensed under the
Creative Commons Attribution 3.0 Unported license; the license text embedded in
each file's comment header is kept intact. Attribution: filter lists by
The EasyList Authors (https://easylist.to/).

Snapshot: 2026-10-31 (easylist.txt version 202610311716).

Refresh manually with `sh tools/ablock/fetch.sh` (needs network); the files are
committed so builds are reproducible offline. The `!` comment headers (version,
commit, license) are skipped by the parser but retained in the files.
