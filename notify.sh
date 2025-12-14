#!/bin/bash
# Notification sender
# Usage: notify.sh "Title" "Body" [duration] [urgency]

TITLE="${1:-Notification}"
BODY="${2:-}"
DURATION="${3:-5000}"
URGENCY="${4:-normal}"

DURATION=$(echo "$DURATION" | tr -d '"')

curl -s -X POST http://localhost:8888/notify \
  -H "Content-Type: application/json" \
  -d "{
    \"title\": \"$TITLE\",
    \"body\": \"$BODY\",
    \"duration\": $DURATION,
    \"urgency\": \"$URGENCY\"
  }" > /dev/null

exit 0
