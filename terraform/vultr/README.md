# Deploying strfry to vultr using terraform

## Install terraform

 1. Add the Terraform GPG key to your server

    ' $ curl -fsSL https://apt.releases.hashicorp.com/gpg | sudo apt-key add -'

 2. Add the official Terraform repository to your APT sources

    ' $ sudo apt-add-repository "deb [arch=amd64] https://apt.releases.hashicorp.com focal main"'

 3. Update the server packages

    ' $ sudo apt update'

 4. Install Terraform on the server

    ' $ sudo apt install terraform'


## Activate Your Vultr API Key

Activate and Copy your Vultr API Key from the Vultr [Customer Portal Settings Page](https://my.vultr.com/settings/#settingsapi)

## Edit Configuration Files

1. Edit terraform.tfvars (see terraform.tfvars.example)
   
    ```
    VULTR_API_KEY   = "EGJGEJIGJKSDGJKSDKSDGJKLDG444JLKG"
    region          = "ewr"
    plan            = "vhp-1c-2gb-amd"
    os              = 447
    label           = "relay.example.com"
    hostname        = "relay"
    script_filename = "startup.freebsd.sh"
    ssh_key_filename= "~/.ssh/id.pub"       
    ```
    
2. Edit Provisioning Scripts
   Find the variables that affect the provisioning at the top of startup.freebsd.sh

    ```
    # change
    domain="relay.example.com"
    email="user@example.com"
    pkgpath="http://download.example.com/downloads/"
    pkgfile="strfry-0.9.6.pkg"
    ```
3. Changing Terraform Plan to Use Ubuntu

    terraform.tfvars
    ```
    os              = 1743
    script_filename = "startup.ubuntu.sh"

    ``` 
    startup.ubuntu.sh 
    ```
    # change
    domain="relay.example.com"
    email="user@example.com"
    pkgpath="http://download.example.com/downloads/"
    pkgfile="strfry_0.9.6-1_amd64.deb"
    ```
    main.tf
    The way Vultr configures the server depends on the operating system.

    * Linux servers use cloud-init.
    * BSD-based servers use boot scripts.

    ```
    # provision script for freebsd
    #  script_id             = vultr_startup_script.startup.id

    # provision script for ubuntu
      user_data             = "${file("${var.script_filename}")}"

    ```
## Initialize Plan 

  ' terraform init '

## Test Plan 

  ' terraform plan '
  
## Execute Plan

  ' terraform apply  -auto-approve  '

## Destroy Plan

  ' terraform destroy  -auto-approve  '
