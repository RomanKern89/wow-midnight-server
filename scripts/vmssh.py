"""One-shot SSH command runner for the server host.

Credentials come from environment variables — NEVER hard-code them.

    export TC_SSH_HOST=10.0.0.10
    export TC_SSH_USER=root
    export TC_SSH_PASS='your-password'     # or use TC_SSH_KEY for a key file
    python vmssh.py "uptime"

Prints stdout+stderr. UTF-8 safe on legacy consoles.
"""
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

import paramiko

HOST = os.environ.get("TC_SSH_HOST", "")
USER = os.environ.get("TC_SSH_USER", "root")
PASSWORD = os.environ.get("TC_SSH_PASS")
KEYFILE = os.environ.get("TC_SSH_KEY")


def run(cmd: str, timeout: int = 110) -> int:
    if not HOST:
        print("Set TC_SSH_HOST (and TC_SSH_PASS or TC_SSH_KEY).", file=sys.stderr)
        return 2
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    connect_kwargs = {"username": USER, "timeout": 20}
    if KEYFILE:
        connect_kwargs["key_filename"] = KEYFILE
    else:
        connect_kwargs["password"] = PASSWORD
    client.connect(HOST, **connect_kwargs)
    try:
        _, out, err = client.exec_command(cmd, timeout=timeout)
        stdout = out.read().decode("utf-8", errors="replace")
        stderr = err.read().decode("utf-8", errors="replace")
        rc = out.channel.recv_exit_status()
        if stdout:
            print(stdout)
        if stderr:
            print("[stderr]", stderr, file=sys.stderr)
        print(f"[rc={rc}]")
        return rc
    finally:
        client.close()


if __name__ == "__main__":
    command = sys.argv[1]
    tmo = int(sys.argv[2]) if len(sys.argv) > 2 else 110
    sys.exit(run(command, tmo))
