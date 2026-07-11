#!/usr/bin/env bash
# Block until MySQL answers.
set -euo pipefail
: "${DB_HOST:=mysql}" "${DB_USER:=trinity}" "${DB_PASS:=trinity}"
echo "Waiting for MySQL at ${DB_HOST}..."
for i in $(seq 1 60); do
  if mysql -h "$DB_HOST" -u "$DB_USER" -p"$DB_PASS" -e "SELECT 1" >/dev/null 2>&1; then
    echo "MySQL is up."; exit 0
  fi
  sleep 3
done
echo "MySQL did not become ready in time." >&2
exit 1
