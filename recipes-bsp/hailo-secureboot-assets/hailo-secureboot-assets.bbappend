CUSTOMER_CERT = "customer_certificate.bin"
CUSTOMER_KEY = "customer_keypair.pem"
LICENSE = "CLOSED" 
BASE_URI = "file://" 
SRC_URI = "${BASE_URI}${CUSTOMER_CERT} \
	   ${BASE_URI}${CUSTOMER_KEY}" 

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
