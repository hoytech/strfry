#!/bin/sh

# change
domain="relay.example.com"
email="user@example.com"
pkgpath="http://download.example.com/downloads/"
pkgfile="strfry_0.9.6-1_amd64.deb"

# install depends and tools 
apt update && apt install -y --no-install-recommends \
    wget liblmdb0 libflatbuffers1 libsecp256k1-0 libb2-1 libzstd1 \
    nginx certbot python3-certbot-nginx

# setup proxy on 80 or ws://
cat << EOF > /etc/nginx/sites-available/default
server{
    server_name $domain;
    location / {
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header Host \$host;
        proxy_pass http://127.0.0.1:7777;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
    }
}
EOF

cd /tmp
wget $pkgpath/$pkgfile
dpkg -i ./$pkgfile
systemctl restart nginx

# requires dns configured for domain
# certbot register --agree-tos --email $email --non-interactive
# certbot --nginx -d $domain
