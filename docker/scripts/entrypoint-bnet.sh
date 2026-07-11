#!/usr/bin/env bash
set -euo pipefail
: "${DB_HOST:=mysql}" "${DB_USER:=trinity}" "${DB_PASS:=trinity}" "${LOGIN_REST_EXTERNAL_ADDRESS:=127.0.0.1}"

/scripts/wait-for-mysql.sh

mkdir -p /opt/tc/etc /opt/tc/logs

# Self-signed dev certificate for the REST login endpoint (generated once).
if [ ! -f /opt/tc/etc/bnetserver.cert.pem ]; then
  echo ">> Generating self-signed dev certificate"
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout /opt/tc/etc/bnetserver.key.pem \
    -out /opt/tc/etc/bnetserver.cert.pem \
    -subj "/CN=${LOGIN_REST_EXTERNAL_ADDRESS}"
fi

echo ">> Rendering bnetserver.conf"
export DB_HOST DB_USER DB_PASS LOGIN_REST_EXTERNAL_ADDRESS
envsubst < /opt/tc/etc-templates/bnetserver.conf.template > /opt/tc/etc/bnetserver.conf

echo ">> Starting bnetserver"
cd /opt/tc/bin
exec ./bnetserver -c /opt/tc/etc/bnetserver.conf
