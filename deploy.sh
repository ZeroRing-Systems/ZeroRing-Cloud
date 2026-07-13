#!/bin/bash
# ZeroRing Deploy Script — run from your LOCAL machine
# Usage: ./deploy.sh

set -e

VM_IP="20.193.236.244"
SSH_KEY="/home/ifkabir/Documents/SShkeys/zeroring-backend_key.pem"
VM_USER="zeroringadmin"

echo "🚀 Deploying ZeroRing Backend to $VM_IP..."

echo "📥 Syncing local code to VM..."
rsync -avz --exclude="build" --exclude=".git" -e "ssh -i $SSH_KEY" ./ "$VM_USER@$VM_IP:~/ZeroRing-Cloud/"

ssh -i "$SSH_KEY" "$VM_USER@$VM_IP" "
  set -e
  cd ~/ZeroRing-Cloud
  echo '🔨 Building with PostgreSQL...'
  mkdir -p build
  cd build
  cmake .. -DUSE_POSTGRES=ON
  make -j\$(nproc)
  echo '🔄 Restarting service...'
  sudo systemctl restart zeroring
  sleep 2
  sudo systemctl status zeroring --no-pager
  echo '✅ Deploy complete!'
"
