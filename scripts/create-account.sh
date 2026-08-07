#!/usr/bin/env bash
# Create a player account on a running worldserver, ready to log in from a
# retail client.
#
#   ./create-account.sh -e player@example.com -p SomePassword
#   ./create-account.sh -e admin@example.com  -p SomePassword -g 3
#
# WHY AN EMAIL AND NOT A NAME
#   A retail client logs in through Battle.net, so the account has to be a
#   *Battle.net* account, and Battle.net account names are email addresses.
#   `account create <name> <password>` makes a plain game account with no
#   Battle.net link - the client cannot log in with it. TrinityCore is explicit
#   about this: `account create` refuses any name containing '@' and tells you
#   to use the bnet commands, while `bnetaccount create` requires one.
#   `bnetaccount create` also creates the linked game account for you.
#
# HOW IT TALKS TO THE SERVER
#   Over SOAP, which worldserver exposes when worldserver.conf has:
#       SOAP.Enabled = 1
#       SOAP.IP      = "127.0.0.1"
#       SOAP.Port    = 7878
#   SOAP.IP defaults to loopback, so run this ON the server host (or tunnel).
#   The SOAP user is an existing GM game account with console rights.
set -euo pipefail

SOAP_HOST="${SOAP_HOST:-127.0.0.1}"
SOAP_PORT="${SOAP_PORT:-7878}"
SOAP_USER="${SOAP_USER:-}"
SOAP_PASS="${SOAP_PASS:-}"
EMAIL=""; PASSWORD=""; GMLEVEL="0"; REALM_ID="-1"

usage() {
    cat >&2 <<EOF
Usage: $0 -e <email> -p <password> [-g <gmlevel>] [-r <realmId>]

  -e  Battle.net account name - must be an email address
  -p  password
  -g  GM level for the game account: 0 player (default), 1 moderator,
      2 gamemaster, 3 administrator
  -r  realm id to grant the GM level on, -1 = all realms (default)

SOAP connection (environment, or edit the defaults at the top):
  SOAP_HOST (default 127.0.0.1)   SOAP_PORT (default 7878)
  SOAP_USER / SOAP_PASS           an existing GM game account

Example:
  SOAP_USER='1#1' SOAP_PASS='secret' $0 -e player@example.com -p hunter2
EOF
    exit 2
}

while getopts ':e:p:g:r:h' opt; do
    case "$opt" in
        e) EMAIL="$OPTARG" ;;
        p) PASSWORD="$OPTARG" ;;
        g) GMLEVEL="$OPTARG" ;;
        r) REALM_ID="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

[ -n "$EMAIL" ] && [ -n "$PASSWORD" ] || usage
[ -n "$SOAP_USER" ] && [ -n "$SOAP_PASS" ] || { echo "ERROR: set SOAP_USER and SOAP_PASS (an existing GM account)." >&2; exit 2; }
command -v curl >/dev/null || { echo "ERROR: curl is required." >&2; exit 2; }

case "$EMAIL" in
    *@*.*) : ;;
    *) echo "ERROR: '$EMAIL' is not an email address. Battle.net account names must be emails." >&2; exit 2 ;;
esac
case "$GMLEVEL" in 0|1|2|3) : ;; *) echo "ERROR: -g must be 0, 1, 2 or 3." >&2; exit 2 ;; esac

xml_escape() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g' -e "s/'/\&apos;/g"; }

# Runs one console command. Prints the server's reply on stdout.
soap() {
    local cmd escaped body http reply
    cmd="$1"
    escaped=$(printf '%s' "$cmd" | xml_escape)
    body="<?xml version=\"1.0\" encoding=\"utf-8\"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\">
<SOAP-ENV:Body><ns1:executeCommand xmlns:ns1=\"urn:TC\"><command>${escaped}</command></ns1:executeCommand></SOAP-ENV:Body></SOAP-ENV:Envelope>"

    reply=$(curl -s -m 30 -w '\n%{http_code}' \
                 -u "${SOAP_USER}:${SOAP_PASS}" \
                 -H 'Content-Type: application/xml' \
                 -d "$body" "http://${SOAP_HOST}:${SOAP_PORT}/" ) || {
        echo "ERROR: cannot reach worldserver SOAP at ${SOAP_HOST}:${SOAP_PORT}." >&2
        echo "       Is the server running, is SOAP.Enabled = 1, and are you on the server host?" >&2
        exit 1
    }
    http=$(printf '%s' "$reply" | tail -n1)
    body=$(printf '%s' "$reply" | sed '$d')

    if [ "$http" = "401" ]; then
        echo "ERROR: SOAP rejected SOAP_USER='${SOAP_USER}'. It must be an existing GM game account" >&2
        echo "       (the '<id>#<n>' form), not the Battle.net email." >&2
        exit 1
    fi

    # <result> spans several lines, so collapse the document onto one line
    # before extracting it, then put the line breaks back.
    local flat
    flat=$(printf '%s' "$body" | tr '\n' '\001')

    # Success -> <result>...</result>; failure -> a SOAP <faultstring>.
    if printf '%s' "$flat" | grep -q '<faultstring>'; then
        printf '%s' "$flat" | sed -n 's/.*<faultstring>\(.*\)<\/faultstring>.*/\1/p' | tr '\001' '\n'
        return 1
    fi
    printf '%s' "$flat" \
        | sed -n 's/.*<result>\(.*\)<\/result>.*/\1/p' \
        | tr '\001' '\n' \
        | sed -e 's/&#xD;//g' -e 's/&lt;/</g' -e 's/&gt;/>/g' -e 's/&quot;/"/g' -e 's/&apos;/'"'"'/g' -e 's/&amp;/\&/g'
}

echo ">> Creating Battle.net account '$EMAIL'"
if ! out=$(soap "bnetaccount create ${EMAIL} ${PASSWORD}"); then
    echo "FAILED: $out" >&2
    exit 1
fi
printf '%s\n' "$out" | sed 's/^/   /'

# The linked game account is auto-named "<bnetId>#<index>"; ask the server
# rather than guessing it.
echo ">> Looking up the linked game account"
if ! list=$(soap "bnetaccount listgameaccounts ${EMAIL}"); then
    echo "WARNING: could not list game accounts: $list" >&2
    list=""
fi
printf '%s\n' "$list" | sed 's/^/   /'
GAME_ACCOUNT=$(printf '%s\n' "$list" | grep -oE '[0-9]+#[0-9]+' | head -n1 || true)

if [ -n "$GAME_ACCOUNT" ]; then
    echo ">> Linked game account: $GAME_ACCOUNT"
else
    echo "WARNING: could not determine the game account name." >&2
fi

if [ "$GMLEVEL" != "0" ]; then
    if [ -z "$GAME_ACCOUNT" ]; then
        echo "ERROR: cannot grant GM level without the game account name." >&2
        echo "       Run manually:  account set gmlevel <account> ${GMLEVEL} ${REALM_ID}" >&2
        exit 1
    fi
    echo ">> Granting GM level ${GMLEVEL} on realm ${REALM_ID}"
    if ! out=$(soap "account set gmlevel ${GAME_ACCOUNT} ${GMLEVEL} ${REALM_ID}"); then
        echo "FAILED: $out" >&2
        exit 1
    fi
    printf '%s\n' "$out" | sed 's/^/   /'
    echo "   NOTE: a logged-in session keeps its old permissions - log out and back in."
fi

cat <<EOF

=====================================================================
 Account ready.

   Login (in the client) : ${EMAIL}
   Password              : ${PASSWORD}
   Game account          : ${GAME_ACCOUNT:-<unknown>}
   GM level              : ${GMLEVEL}

 Log in with the EMAIL, not the game account name.
 Point the client at this server with scripts/setup-client.ps1.
=====================================================================
EOF
