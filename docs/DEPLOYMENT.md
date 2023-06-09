# Deployment

This document will explain how to get an instance of strfry installed on its own VM, for example on a cloud provider like DigitalOcean, Linode, AWS or Vultr.

## Starting & Endpoint

We'll assume you have a running VPS that you have access to:

- You can ssh to the node
- You have the IP address of the node
- You can use some kind of DNS registrar to point some domain at the node

The goal is to have:

- A running strfry relay
- With a letsencrypt certificate

## Provision your server

The remainder of the document assumes a plain VPS.

Vultr - $12/mo
- High performance Intel
- 1 vCPU
- 2048 MB Ram
- 50 GB NVMe SSD
- OS Ubuntu 22.04 LTS
- Disable automatic backup

## Point DNS at your server

While you wait for your server to provision, go to your DNS provider and point an address at it.  An A record of `relay.yourdomain.com` should point to the IP address of your VPS.

## Connect to your server


        # Update deps
        sudo apt update

        # Download strfry
        git clone https://github.com/hoytech/strfry.git
        cd strfry

        # Install complication dependencies
        sudo apt install -y git build-essential libyaml-perl libtemplate-perl libregexp-grammars-perl libssl-dev zlib1g-dev liblmdb-dev libflatbuffers-dev libsecp256k1-dev libzstd-dev

        # Build it
        git submodule update --init
        make setup-golpe
        make -j2

        # Go get coffee, this takes a few minutes on a single vCPU

        # Install strfry
        cp strfry /usr/local/bin

        # Install web hosting dependencies
        sudo apt install nginx certbot python3-certbot-nginx

        # Remove the default nginx file
        sudo rm -rf /etc/nginx/sites-available/default

        # Provide the following settings file
        sudo vim /etc/nginx/sites-available/default

        sudo service nginx restart

Note, at this point your nginx will just try to pass all connections to a nonexistent strfry endpoint, get frustrated, and refuse the connection.

### Configure Strfry

- Create user 

        sudo useradd -M -s /usr/sbin/nologin strfry

- Create data directory

        sudo mkdir /var/lib/strfry
        sudo chown strfry:strfry /var/lib/strfry
        sudo chmod 755 /var/lib/strfry 


### Sample NGINX Starter Config

Note here you'll use the DNS name you configured above

        server{
            server_name relay.yourdomain.com;
            location / {
                proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
                proxy_set_header Host $host;
                proxy_pass http://127.0.0.1:7777;
                proxy_http_version 1.1;
                proxy_set_header Upgrade $http_upgrade;
                proxy_set_header Connection "upgrade";
            }
        }

### Install strfry.conf

Edit the `db = "./strfry-db/"` line to: `db = "/var/lib/strfry/"`

Copy the strfry.conf file to /etc and change ownership:

        sudo cp strfry.conf /etc/strfry.conf
        sudo chown strfry:strfry /etc/strfry.conf

### Install Systemd File

Put the following file at /etc/systemd/system/strfry.service

        [Unit]
        Description=strfry relay service

        [Service]
        User=strfry
        ExecStart=/usr/local/bin/strfry relay
        Restart=on-failure
        RestartSec=5
        ProtectHome=yes
        NoNewPrivileges=yes
        ProtectSystem=full
        LimitCORE=1000000000

        [Install]
        WantedBy=multi-user.target
    
Now enable this service and start it

        sudo systemctl enable strfry.service
        sudo systemctl start strfry

        sudo systemctl status strfry

You can curl to ensure things are running:

        curl localhost:7777 

Tells you that strfry is running

        curl localhost:80

Tells you that nginx is running

### Firewall and Certificate

Now let's open the port to the outside world:


        sudo ufw allow 'Nginx Full'
        sudo ufw status

        sudo certbot --nginx -d relay.yourdomain.com

### Next steps 

At this point you should have a running relay.  Point your client at it, tell a few friends, post some notes, or whatever you want.
