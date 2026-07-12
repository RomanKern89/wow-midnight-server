#!/usr/bin/env bash
# One-shot DB bootstrap: create schemas, import base + user world dump, apply fixes.
# Idempotent: re-running skips steps that are already done.
set -euo pipefail

: "${DB_HOST:=mysql}" "${DB_USER:=trinity}" "${DB_PASS:=trinity}" "${MYSQL_ROOT_PASSWORD:=rootpass}"
SQLBASE=/opt/tc/sql-base

# Password via MYSQL_PWD (no "-p" on the command line -> no insecure-password warning).
root_sql() { MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -h "$DB_HOST" -u root "$@"; }
user_sql() { MYSQL_PWD="$DB_PASS" mysql -h "$DB_HOST" -u "$DB_USER" "$@"; }

echo ">> Waiting for MySQL (root)..."
for i in $(seq 1 60); do root_sql -e "SELECT 1" >/dev/null 2>&1 && break; sleep 3; done

echo ">> Creating databases + grants"
root_sql <<SQL
CREATE DATABASE IF NOT EXISTS auth       DEFAULT CHARSET utf8mb4;
CREATE DATABASE IF NOT EXISTS characters DEFAULT CHARSET utf8mb4;
CREATE DATABASE IF NOT EXISTS world      DEFAULT CHARSET utf8mb4;
CREATE DATABASE IF NOT EXISTS hotfixes   DEFAULT CHARSET utf8mb4;
GRANT ALL ON auth.*       TO '${DB_USER}'@'%';
GRANT ALL ON characters.* TO '${DB_USER}'@'%';
GRANT ALL ON world.*      TO '${DB_USER}'@'%';
GRANT ALL ON hotfixes.*   TO '${DB_USER}'@'%';
FLUSH PRIVILEGES;
SQL

have_tables() { [ "$(user_sql -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='$1'")" -gt 0 ]; }

# --- auth / characters base schema (ships with the TrinityCore source) ---
if ! have_tables auth && [ -f "$SQLBASE/base/auth_database.sql" ]; then
  echo ">> Importing auth base schema"; user_sql auth < "$SQLBASE/base/auth_database.sql"
fi
if ! have_tables characters && [ -f "$SQLBASE/base/characters_database.sql" ]; then
  echo ">> Importing characters base schema"; user_sql characters < "$SQLBASE/base/characters_database.sql"
fi

# --- world DB: YOUR dump (Blizzard-derived; we ship none — see docker/import/) ---
if ! have_tables world; then
  DUMP=$(ls -1 /import/*.sql 2>/dev/null | head -1 || true)
  if [ -n "$DUMP" ]; then
    echo ">> Importing world dump: $DUMP  (this can take a while)"
    user_sql world < "$DUMP"
  else
    echo "!! No world DB dump found in ./import — the world DB is EMPTY."
    echo "!! Drop a build-68275 world dump into docker/import/ and re-run 'docker compose up db-init'."
  fi
else
  echo ">> world DB already populated — skipping dump import."
fi

# --- our community fixes (always safe to re-apply: INSERT IGNORE / idempotent UPDATEs) ---
if have_tables world; then
  for f in /fixes/*.sql; do
    [ -e "$f" ] || continue
    echo ">> Applying fix: $(basename "$f")"
    user_sql world < "$f" || echo "   (warning: $(basename "$f") reported errors — continuing)"
  done
fi

echo ">> DB init complete."
