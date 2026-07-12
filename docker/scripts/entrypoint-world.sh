#!/usr/bin/env bash
set -euo pipefail
: "${DB_HOST:=mysql}" "${DB_USER:=trinity}" "${DB_PASS:=trinity}" "${REALM_ADDRESS:=127.0.0.1}"

/scripts/wait-for-mysql.sh

echo ">> Rendering worldserver.conf"
mkdir -p /opt/tc/etc /opt/tc/logs
export DB_HOST DB_USER DB_PASS
envsubst < /opt/tc/etc-templates/worldserver.conf.template > /opt/tc/etc/worldserver.conf

echo ">> Pointing realm ${REALM_ADDRESS} in auth.realmlist"
MYSQL_PWD="$DB_PASS" mysql -h "$DB_HOST" -u "$DB_USER" auth -e \
  "UPDATE realmlist SET address='${REALM_ADDRESS}', localAddress='${REALM_ADDRESS}', port=8085 WHERE id=1;" \
  2>/dev/null || echo "   (realmlist not ready yet — will be set once auth base exists)"

if [ ! -e /opt/tc/data/maps ] && [ ! -e /opt/tc/data/Maps ]; then
  echo "!! WARNING: /opt/tc/data has no 'maps' — mount your client-extracted data"
  echo "!! (dbc/maps/vmaps/mmaps) into ./data. worldserver will fail to start without it."
fi

echo ">> Starting worldserver"
cd /opt/tc/bin
exec ./worldserver -c /opt/tc/etc/worldserver.conf
