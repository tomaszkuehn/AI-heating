#!/usr/bin/env bash
set -e
cd ~
mkdir -p esp && cd esp
if [ ! -d esp-idf ]; then
  echo "=== cloning esp-idf v5.3.2 ==="
  git clone --recursive https://github.com/espressif/esp-idf.git -b v5.3.2 esp-idf
fi
cd esp-idf
echo "=== install.sh esp32 ==="
./install.sh esp32
echo "=== DONE_IDF_INSTALL ==="
