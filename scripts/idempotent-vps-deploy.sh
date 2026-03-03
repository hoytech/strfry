#!/bin/bash

## This script is a contribution from @chr15m
## For more information, see the following pull request: https://github.com/hoytech/strfry/pull/151

set -e

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "Deploys strfry to a fresh Ubuntu VPS."
    echo "(Installs nginx, acme.sh, and builds strfry from scratch)"
    echo
    echo "Usage: $0 HOST [ADMIN_EMAIL] [ADMIN_PUBKEY]"
    echo "Example: $0 my-relay.example.com admin@example.com <32-byte-hex-pubkey>"
    exit 1
fi

HOST="$1"
ADMIN_EMAIL="${2:-admin@$HOST}"
ADMIN_PUBKEY="${3:-}"

# The rest of the script will be executed on the remote host
ssh "root@$HOST" bash -s -- "$HOST" "$ADMIN_EMAIL" "$ADMIN_PUBKEY" << 'EOF'
set -e
set -x

HOST="$1"
ADMIN_EMAIL="$2"
ADMIN_PUBKEY="$3"

# Variables
STRFRY_USER="strfry"
STRFRY_GROUP="strfry"
STRFRY_HOME="/var/lib/strfry"
STRFRY_DB_DIR="${STRFRY_HOME}/strfry-db"
STRFRY_CONFIG="/etc/strfry.conf"
STRFRY_BUILD_DIR="/tmp/strfry-build"
STRFRY_BINARY_PATH="/usr/local/bin/strfry"
STRFRY_REPO_URL="https://github.com/hoytech/strfry.git"
STRFRY_REPO_VERSION="master"

echo "--- Starting strfry deployment on ${HOST} ---"

# 1. Install dependencies
echo "--- Installing dependencies ---"
apt-get update
apt-get install -y --no-install-recommends \
    git g++ make pkg-config libtool ca-certificates \
    libssl-dev zlib1g-dev liblmdb-dev libflatbuffers-dev \
    libsecp256k1-dev libzstd-dev

# 1.5. Configure swap
echo "--- Checking and configuring swap space ---"
if [ $(free -m | awk '/^Swap/ {print $2}') -lt 4000 ]; then
    echo "Swap space is less than 4G. Creating a 4G swapfile."
    if [ -f /swapfile ]; then
        echo "/swapfile already exists, assuming it is for swap."
    else
        fallocate -l 4G /swapfile
        chmod 600 /swapfile
        mkswap /swapfile
    fi
    swapon /swapfile || true
    if ! grep -q "^/swapfile" /etc/fstab; then
        echo '/swapfile none swap sw 0 0' >> /etc/fstab
    fi
else
    echo "Swap space is sufficient."
fi
free -h

# 2. Create user and group
echo "--- Creating strfry user and group ---"
if ! getent group "$STRFRY_GROUP" >/dev/null; then
    addgroup --system "$STRFRY_GROUP"
fi

if ! id "$STRFRY_USER" >/dev/null 2>&1; then
    adduser --system --ingroup "$STRFRY_GROUP" --home "$STRFRY_HOME" --shell /usr/sbin/nologin --no-create-home "$STRFRY_USER"
fi

# 3. Create directories
echo "--- Creating directories ---"
mkdir -p "$STRFRY_HOME"
mkdir -p "$STRFRY_DB_DIR"
chown -R "$STRFRY_USER":"$STRFRY_GROUP" "$STRFRY_HOME"
chmod 0750 "$STRFRY_DB_DIR"

# 4. Build strfry
echo "--- Cloning and building strfry ---"
if [ -d "$STRFRY_BUILD_DIR/.git" ]; then
    echo "Updating existing repository..."
    cd "$STRFRY_BUILD_DIR"
    git fetch
    git reset --hard "origin/$STRFRY_REPO_VERSION"
    if git submodule status --recursive | grep -qE '^[+-U]'; then
        git submodule update --init --recursive
    fi
    make -j$(nproc)
else
    echo "Cloning fresh repository..."
    rm -rf "$STRFRY_BUILD_DIR"
    git clone --branch "$STRFRY_REPO_VERSION" "$STRFRY_REPO_URL" "$STRFRY_BUILD_DIR"
    cd "$STRFRY_BUILD_DIR"
    git submodule update --init --recursive
    make setup-golpe
    make -j$(nproc)
fi

# 5. Install strfry
echo "--- Installing strfry binary ---"
systemctl stop strfry || true
cp "$STRFRY_BUILD_DIR/strfry" "$STRFRY_BINARY_PATH"
chmod 755 "$STRFRY_BINARY_PATH"

# 6. Create config file
echo "--- Creating config file ---"
echo "Creating default config file at ${STRFRY_CONFIG}"
cat > "$STRFRY_CONFIG" << CONFIG_EOF
##
## Default strfry config
##

# Directory that contains the strfry LMDB database (restart required)
db = "${STRFRY_DB_DIR}/"

dbParams {
    # Maximum number of threads/processes that can simultaneously have LMDB transactions open (restart required)
    maxreaders = 256

    # Size of mmap() to use when loading LMDB (default is 10TB, does *not* correspond to disk-space used) (restart required)
    mapsize = 10995116277760

    # Disables read-ahead when accessing the LMDB mapping. Reduces IO activity when DB size is larger than RAM. (restart required)
    noReadAhead = false
}

events {
    # Maximum size of normalised JSON, in bytes
    maxEventSize = 65536

    # Events newer than this will be rejected
    rejectEventsNewerThanSeconds = 900

    # Events older than this will be rejected
    rejectEventsOlderThanSeconds = 94608000

    # Ephemeral events older than this will be rejected
    rejectEphemeralEventsOlderThanSeconds = 60

    # Ephemeral events will be deleted from the DB when older than this
    ephemeralEventsLifetimeSeconds = 300

    # Maximum number of tags allowed
    maxNumTags = 2000

    # Maximum size for tag values, in bytes
    maxTagValSize = 1024
}

relay {
    # Interface to listen on. Use 0.0.0.0 to listen on all interfaces (restart required)
    bind = "127.0.0.1"

    # Port to open for the nostr websocket protocol (restart required)
    port = 7777

    # Set OS-limit on maximum number of open files/sockets (if 0, don't attempt to set) (restart required)
    nofiles = 65536

    # HTTP header that contains the client's real IP, before reverse proxying (ie x-real-ip) (MUST be all lower-case)
    realIpHeader = "x-real-ip"

    info {
        # NIP-11: Name of this server. Short/descriptive (< 30 characters)
        # PLEASE CUSTOMIZE
        name = "strfry relay at ${HOST}"

        # NIP-11: Detailed information about relay, free-form
        # PLEASE CUSTOMIZE
        description = "A strfry relay running at ${HOST}"

        # NIP-11: Administrative nostr pubkey, for contact purposes
        # PLEASE CUSTOMIZE
        pubkey = "${ADMIN_PUBKEY}"

        # NIP-11: Alternative administrative contact (email, website, etc)
        # PLEASE CUSTOMIZE
        contact = "${ADMIN_EMAIL}"

        # NIP-11: URL pointing to an image to be used as an icon for the relay
        icon = ""

        # List of supported lists as JSON array, or empty string to use default. Example: "[1,2]"
        nips = ""
    }

    # Maximum accepted incoming websocket frame size (should be larger than max event) (restart required)
    maxWebsocketPayloadSize = 131072

    # Maximum number of filters allowed in a REQ
    maxReqFilterSize = 200

    # Websocket-level PING message frequency (should be less than any reverse proxy idle timeouts) (restart required)
    autoPingSeconds = 55

    # If TCP keep-alive should be enabled (detect dropped connections to upstream reverse proxy)
    enableTcpKeepalive = false

    # How much uninterrupted CPU time a REQ query should get during its DB scan
    queryTimesliceBudgetMicroseconds = 10000

    # Maximum records that can be returned per filter
    maxFilterLimit = 500

    # Maximum number of subscriptions a client can have open at any time.
    # A subscription is created by a REQ message and has a unique ID.
    maxSubsPerConnection = 20

    writePolicy {
        # If non-empty, path to an executable script that implements the writePolicy plugin logic
        plugin = ""
    }

    compression {
        # Use permessage-deflate compression if supported by client. Reduces bandwidth, but slight increase in CPU (restart required)
        enabled = true

        # Maintain a sliding window buffer for each connection. Improves compression, but uses more memory (restart required)
        slidingWindow = true
    }

    logging {
        # Dump all incoming messages
        dumpInAll = false

        # Dump all incoming EVENT messages
        dumpInEvents = false

        # Dump all incoming REQ/CLOSE messages
        dumpInReqs = false

        # Log performance metrics for initial REQ database scans
        dbScanPerf = false

        # Log reason for invalid event rejection? Can be disabled to silence excessive logging
        invalidEvents = true
    }

    numThreads {
        # Ingester threads: route incoming requests, validate events/sigs (restart required)
        ingester = 3

        # reqWorker threads: Handle initial DB scan for events (restart required)
        reqWorker = 3

        # reqMonitor threads: Handle filtering of new events (restart required)
        reqMonitor = 3

        # negentropy threads: Handle negentropy protocol messages (restart required)
        negentropy = 2
    }

    negentropy {
        # Support negentropy protocol messages
        enabled = true

        # Maximum records that sync will process before returning an error
        maxSyncEvents = 1000000
    }
}
CONFIG_EOF
    chown root:"$STRFRY_GROUP" "$STRFRY_CONFIG"
    chmod 640 "$STRFRY_CONFIG"


# 7. Create systemd service
echo "--- Creating systemd service ---"
if [ ! -f /etc/systemd/system/strfry.service ]; then
    echo "Creating systemd service file /etc/systemd/system/strfry.service"
    cat > /etc/systemd/system/strfry.service << 'SERVICE_EOF'
[Unit]
Description=strfry nostr relay
After=network.target

[Service]
User=strfry
Group=strfry
ExecStart=/usr/local/bin/strfry relay --config /etc/strfry.conf
Restart=on-failure
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
SERVICE_EOF
else
    echo "Service file /etc/systemd/system/strfry.service already exists, skipping creation."
fi

# 8. Enable and start service
echo "--- Enabling and starting strfry service ---"
systemctl daemon-reload
systemctl enable strfry
systemctl restart strfry

# 9. Configure nginx and SSL
echo "--- Configuring nginx and SSL ---"
apt-get install -y nginx socat curl

# Install acme.sh for root user
if [ ! -f /root/.acme.sh/acme.sh ]; then
    echo "Installing acme.sh..."
    curl -o /root/acme.sh https://raw.githubusercontent.com/Neilpang/acme.sh/master/acme.sh
    chmod +x /root/acme.sh
    cd /root
    ./acme.sh --install --home /root/.acme.sh --accountemail "${ADMIN_EMAIL}"
    rm ./acme.sh
    /root/.acme.sh/acme.sh --upgrade --auto-upgrade
    /root/.acme.sh/acme.sh --set-default-ca --server letsencrypt
else
    echo "acme.sh already installed."
fi

# If no cert, configure for http challenge.
if [ ! -f /root/.acme.sh/${HOST}_ecc/${HOST}.cer ]; then
    echo "Creating nginx site config for http challenge"
    cat > /etc/nginx/sites-available/strfry << NGINX_EOF
map \$http_upgrade \$connection_upgrade {
    default upgrade;
    ''      close;
}

server {
    listen 80;
    server_name ${HOST};

    location /.well-known/acme-challenge/ {
        root /var/www/html;
    }

    location / {
        proxy_pass http://127.0.0.1:7777;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection \$connection_upgrade;
        proxy_set_header Host \$host;
        proxy_set_header x-real-ip \$remote_addr;
        proxy_set_header x-forwarded-for \$proxy_add_x_forwarded_for;
        proxy_set_header x-forwarded-proto \$scheme;
    }
}
NGINX_EOF
    mkdir -p /var/www/html
    ln -sf /etc/nginx/sites-available/strfry /etc/nginx/sites-enabled/
    if [ -f /etc/nginx/sites-enabled/default ]; then
        rm -f /etc/nginx/sites-enabled/default
    fi
    systemctl restart nginx

    echo "Obtaining SSL certificate for ${HOST}"
    /root/.acme.sh/acme.sh --issue -d "${HOST}" -w /var/www/html
fi

# Now that cert should exist, create final config
echo "Creating final nginx config for strfry"
cat > /etc/nginx/sites-available/strfry << NGINX_EOF
map \$http_upgrade \$connection_upgrade {
    default upgrade;
    ''      close;
}

server {
    listen 80;
    server_name ${HOST};
    return 301 https://\$host\$request_uri;
}

server {
    listen 443 ssl http2;
    server_name ${HOST};

    ssl_certificate /root/.acme.sh/${HOST}_ecc/fullchain.cer;
    ssl_certificate_key /root/.acme.sh/${HOST}_ecc/${HOST}.key;
    ssl_trusted_certificate /root/.acme.sh/${HOST}_ecc/ca.cer;

    location / {
        proxy_pass http://127.0.0.1:7777;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection \$connection_upgrade;
        proxy_set_header Host \$host;
        proxy_set_header x-real-ip \$remote_addr;
        proxy_set_header x-forwarded-for \$proxy_add_x_forwarded_for;
        proxy_set_header x-forwarded-proto \$scheme;
    }
}
NGINX_EOF
ln -sf /etc/nginx/sites-available/strfry /etc/nginx/sites-enabled/
if [ -f /etc/nginx/sites-enabled/default ]; then
    rm -f /etc/nginx/sites-enabled/default
fi
systemctl restart nginx

echo "--- strfry deployment complete ---"
echo
echo "File locations:"
echo "  - Binary:      ${STRFRY_BINARY_PATH}"
echo "  - Config:      ${STRFRY_CONFIG}"
echo "  - Database:    ${STRFRY_DB_DIR}"
echo "  - Service:     /etc/systemd/system/strfry.service"
echo
echo "Logs are managed by systemd's journal."
echo "View logs with: journalctl -u strfry -f"
echo
echo "To stop:  systemctl stop strfry"
echo "To start: systemctl start strfry"
echo "To check status: systemctl status strfry"

EOF
