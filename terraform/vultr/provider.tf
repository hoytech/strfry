terraform {

   # Use the latest provider release: https://github.com/vultr/terraform-provider-vultr/releases
   required_providers {
     vultr = {
       source  = "vultr/vultr"
       version = ">= 2.15.1"
     }
   }

   # Configure the S3 backend
#  backend "s3" {
#    bucket                      = "terraform-state-strfry"
#    key                         = "terraform.tfstate"
#    endpoint                    = "ewr1.vultrobjects.com"
#    region                      = "us-east-1"
#    skip_credentials_validation = true
#  }
}

provider "vultr" {
  api_key = var.VULTR_API_KEY
  # Set the API rate limit
  rate_limit  = 700
  retry_limit = 3
}

