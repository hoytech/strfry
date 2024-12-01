variable "VULTR_API_KEY" {
    description   = "Vultr API token"
    type          = string
}

variable "os" {
    description   = "OS"
    type          = string
}

variable "plan" {
    description   = "Plan"
    type          = string
}

variable "label" {
    description   = "Server Name Labeling"
    type          = string
}

variable "region" {
    description   = "Region"
    type          = string
}

variable "hostname" {
    description   = "Resource Hostname"
    type          = string
}

variable "script_filename" {
    description   = "Provisioning Script"
    type          = string
}

variable "ssh_key_filename" {
    description   = "SSH Key File"
    type          = string
}