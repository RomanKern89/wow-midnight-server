#!/usr/bin/env bash
# One-shot DB bootstrap: create schemas, import base + user world dump, apply fixes.
# Idempotent: re-running skips steps that are already done.
set -euo pipefail

: "${DB_HOST:=mysql}" "${DB_USER:=trinity}" "${DB_PASS:=trinity}" "${MYSQL_ROOT_PASSWORD:=rootpass}"
ROOT=(mysql -h "$DB_HOST" -u root -p"$MYSQL_ROOT_PASSWORD")
SQLBASE=/opt/tc/sql-base

echo ">> Waiting for MySQL (root)..."
for i in $(seq 1 60); do "${ROOT[@]}" -e "SELECT 1" >/dev/null 2>&1 && break; sleep 3; done

echo ">> Creating databases + grants"
"${ROOT[@]}" <<SQL
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

USER=(mysql -h "$DB_HOST" -u "$DB_USER" -p"$DB_PASS")
have_tables() { [ "$("${USER[@]}" -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='$1'")" -gt 0 ]; }

# --- auth / characters base schema (ships with the TrinityCore source) ---
if ! have_tables auth && [ -f "$SQLBASE/base/auth_database.sql" ]; then
  echo ">> Importing auth base schema"; "${USER[@]}" auth < "$SQLBASE/base/auth_database.sql"
fi
if ! have_tables characters && [ -f "$SQLBASE/base/characters_database.sql" ]; then
  echo ">> Importing characters base schema"; "${USER[@]}" characters < "$SQLBASE/base/characters_database.sql"
fi

# --- world DB: YOUR dump (Blizzard-derived; we ship none — see docker/import/) ---
if ! have_tables world; then
  DUMP=$(ls -1 /import/*.sql 2>/dev/null | head -1 || true)
  if [ -n "$DUMP" ]; then
    echo ">> Importing world dump: $DUMP  (this can take a while)"
    "${USER[@]}" world < "$DUMP"
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
    "${USER[@]}" world < "$f" || echo "   (warning: $(basename "$f") reported errors — continuing)"
  done
fi

echo ">> DB init complete."
