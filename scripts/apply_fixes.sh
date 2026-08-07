#!/usr/bin/env bash
# Apply the community fix SQL files to your TrinityCore world database.
#
# Usage:
#   DB_USER=trinity DB_PASS=trinity DB_NAME=world ./apply_fixes.sh
#
# After applying, reload live in the worldserver console:
#   reload quest_template            # for the quest fixes (no restart)
# The GameObject / graveyard / raid-binding changes need a worldserver restart.
set -euo pipefail

DB_USER="${DB_USER:-trinity}"
DB_PASS="${DB_PASS:-trinity}"
DB_NAME="${DB_NAME:-world}"
DB_HOST="${DB_HOST:-127.0.0.1}"

SQL_DIR="$(cd "$(dirname "$0")/../sql" && pwd)"

# Password on the command line is visible in `ps`. Hand it to mysql through the
# environment instead, which mysql reads directly.
export MYSQL_PWD="$DB_PASS"

failed=0
for f in "$SQL_DIR"/*.sql; do
    echo ">> Applying $(basename "$f")"
    # One failing file must not abort the rest, and must not go unnoticed.
    if ! mysql -h "$DB_HOST" -u "$DB_USER" "$DB_NAME" < "$f"; then
        echo "!! FAILED: $(basename "$f")" >&2
        failed=$((failed + 1))
    fi
done

if [ "$failed" -ne 0 ]; then
    echo >&2
    echo "$failed file(s) failed — the database may be partially updated." >&2
    echo "Fix the cause and re-run; every file in sql/ is safe to apply twice." >&2
    exit 1
fi

# sql/hotfixes/ belongs to a DIFFERENT database and is deliberately not part of
# the loop above — applying it to `world` would do nothing useful. Opt in with
# HOTFIXES_DB=hotfixes.
if [ -n "${HOTFIXES_DB:-}" ]; then
    for f in "$SQL_DIR"/hotfixes/*.sql; do
        [ -e "$f" ] || continue
        echo ">> Applying $(basename "$f") to '$HOTFIXES_DB'"
        if ! mysql -h "$DB_HOST" -u "$DB_USER" "$HOTFIXES_DB" < "$f"; then
            echo "!! FAILED: $(basename "$f")" >&2
            exit 1
        fi
    done
else
    echo
    echo "NOTE: sql/hotfixes/ was skipped — those files apply to the hotfixes"
    echo "database, not world. To include them, re-run with HOTFIXES_DB=hotfixes."
fi

echo
echo "Done. Remember: 'reload quest_template' in the worldserver console for the"
echo "quest fixes; everything else needs a worldserver restart."
