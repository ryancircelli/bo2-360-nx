#!/bin/bash
# Laptop poller: check origin/main of bo2-360-nx whenever its head moves. Run: setsid nohup bash poll.sh &
CI=$HOME/bo2-worker/ci
while true; do
  h=$(git ls-remote https://github.com/ryancircelli/bo2-360-nx refs/heads/main | cut -c1-10)
  if [ -n "$h" ] && [ "$h" != "$(cat $CI/polled-main 2>/dev/null)" ]; then
    echo "$h" > $CI/polled-main
    bash $CI/check.sh main > $CI/poll-latest.log 2>&1
    cp $CI/poll-latest.log $CI/results/main-latest.log
  fi
  sleep 120
done
