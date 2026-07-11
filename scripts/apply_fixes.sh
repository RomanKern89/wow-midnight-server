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

for f in "$SQL_DIR"/*.sql; do
    echo ">> Applying $(basename "$f")"
    mysql -h "$DB_HOST" -u "$DB_USER" -p"$DB_PASS" "$DB_NAME" < "$f"
done

echo "Done. Remember: 'reload quest_template' in the worldserver console for quest fixes;"
echo "restart the worldserver for GameObject / graveyard / raid-binding changes."
