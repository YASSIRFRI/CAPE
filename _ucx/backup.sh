#!/usr/bin/env bash
folder=${PWD##*/} 

DATE=`date +%Y%m%d%H%M`

cp -r ~/$folder ~/Dropbox/Backup/$DATE$folder
