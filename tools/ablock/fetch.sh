#!/bin/sh
# Re-download the adblock filter lists into filters/ (manual refresh; the
# checked-in files are what the build ships). Run from the repo root.
set -e
cd "$(dirname "$0")/../.."
# --ssl-no-revoke: Git Bash curl/schannel cannot reach the CRL server here
FETCH="curl -sS --ssl-no-revoke"
$FETCH -o filters/easylist.txt    https://easylist.to/easylist/easylist.txt
$FETCH -o filters/easyprivacy.txt https://easylist.to/easylist/easyprivacy.txt
wc -c filters/easylist.txt filters/easyprivacy.txt
head -4 filters/easylist.txt | sed 's/^/easylist: /'
head -4 filters/easyprivacy.txt | sed 's/^/easyprivacy: /'
