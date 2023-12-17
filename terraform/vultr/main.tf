# Store the New Jersey location code to a variable.
data "vultr_region" "ny" {
  filter {
    name   = "city"
    values = ["New Jersey"]
  }
}

# Store the FreeBSD 13 OS code to a variable.
data "vultr_os" "freebsd" {
  filter {
    name   = "name"
    values = ["FreeBSD 13 x64"]
  }
}

# Store the Ubuntu 22.04 LTS OS code to a variable.
data "vultr_os" "ubuntu" {
  filter {
    name   = "name"
    values = ["Ubuntu 22.04 LTS x64"]
  }
}

resource "vultr_ssh_key" "user" {
  name    = "pub_key"
  ssh_key = "${file("${var.ssh_key_filename}")}"
}

resource "vultr_startup_script" "startup" {
    name = "strfry-deploy"
    script = filebase64("${var.script_filename}")
}

# Deploy a Server using the High Performance, 1 Core, 2 GB RAM plan. 
resource "vultr_instance" "instance" {
  plan                  = var.plan
  region                = var.region
  os_id                 = var.os
  label                 = var.label
  hostname              = var.hostname
  firewall_group_id     = vultr_firewall_group.firewall_grp.id
  ssh_key_ids           = ["${vultr_ssh_key.user.id}"]
  tags                  = ["strfry", "nostr"]
  backups               = "disabled"
  enable_ipv6           = false
  ddos_protection       = false
  activation_email      = false

# provision script for freebsd
#  script_id             = vultr_startup_script.startup.id

# provision script for ubuntu
  user_data             = "${file("${var.script_filename}")}"

#  user_data             = <<-EOF
#cloud-config
# Your cloud-init configuration goes here
#EOF
}

# Display the server IP address when complete.
output "instance_ip" {
  value = vultr_instance.instance.main_ip
}

output "instance_id" {
  value = vultr_instance.instance.id
}

