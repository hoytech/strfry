
# change
domain="relay.example.com"
email="user@example.com"
pkgpath="http://download.example.com/downloads/"
pkgfile="strfry-0.9.6.pkg"

# install depends and tools (TODO: find libstdc++ and remove gcc)
pkg install -y wget openssl lmdb flatbuffers libuv libinotify zstd secp256k1 zlib-ng nginx curl py39-certbot-nginx gcc

# setup proxy on 80 or ws://
cat << EOF > /usr/local/etc/nginx/nginx.conf

#user  nobody;
worker_processes  1;

# This default error log path is compiled-in to make sure configuration parsing
# errors are logged somewhere, especially during unattended boot when stderr
# isn't normally logged anywhere. This path will be touched on every nginx
# start regardless of error log location configured here. See
# https://trac.nginx.org/nginx/ticket/147 for more info. 
#
#error_log  /var/log/nginx/error.log;
#

#pid        logs/nginx.pid;

events {
    worker_connections  1024;
}

http {
    include       mime.types;
    default_type  application/octet-stream;

    #log_format  main  '$remote_addr - $remote_user [$time_local] "$request" '
    #                  '$status $body_bytes_sent "$http_referer" '
    #                  '"$http_user_agent" "$http_x_forwarded_for"';

    #access_log  logs/access.log  main;

    sendfile        on;
    #tcp_nopush     on;

    #keepalive_timeout  0;
    keepalive_timeout  65;


    # HTTPS server
    #
    #server {
    #    listen       443 ssl;
    #    server_name  localhost;

    #    ssl_certificate      cert.pem;
    #    ssl_certificate_key  cert.key;

    #    ssl_session_cache    shared:SSL:1m;
    #    ssl_session_timeout  5m;

    #    ssl_ciphers  HIGH:!aNULL:!MD5;
    #    ssl_prefer_server_ciphers  on;

    #    location / {
    #        root   html;
    #        index  index.html index.htm;
    #    }
    #}

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
}
EOF
# enable nginx
if ! grep -q "nginx_enable=" /etc/rc.conf; then
    echo 'nginx_enable="YES"' >> /etc/rc.conf
fi

# fetch and install strfry (pkg enables by default)
cd /tmp
/usr/local/bin/wget $pkgpath/$pkgfile
pkg add ./$pkgfile

service strfry start
service nginx start

# requires dns configured for domain
# certbot register --agree-tos --email $email --non-interactive
# certbot --nginx  -d $domain
